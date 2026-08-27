# Session View (Phase 3) — Design

**Status:** Approved by user 2026-08-27. Ready for implementation planning.

## Goal

Replace the `PlaceholderView` currently shown under `MainComponent::View::session`
with a real Ableton-Live-style track x scene clip launcher: a grid where each
column is an existing project `Track` and each row is a `Scene`, cells hold
loopable clips that fire independently of the linear timeline, whole scene
rows can be batch-fired, and a `Generator` run's candidates can be dropped in
side by side (one candidate per scene row) so they can be quantize-switched
between for A/B listening.

This is the feature the project's own placeholder text already sketched
(`Source/UI/MainComponent.cpp:204-209`, quoting "a track x scene grid for
launching clips non-linearly... generated candidates able to sit side by
side in the cells for comparison") — this document turns that sketch into a
concrete design.

## Tech Stack

- **Language/standard:** C++20 (`CMakeLists.txt:4-5`).
- **Framework:** JUCE 8.0.6, fetched via CMake `FetchContent` by default, or a
  local checkout via `SCORESMITH_JUCE_PATH` (`CMakeLists.txt:14-23`). Linked
  modules: `juce_audio_utils`, `juce_audio_basics`, `juce_audio_devices`,
  `juce_audio_formats`, `juce_audio_processors`, `juce_dsp`, `juce_core`,
  `juce_events`, `juce_data_structures`, `juce_graphics`, `juce_gui_basics`,
  `juce_gui_extra` (`CMakeLists.txt:70-87`).
- **Build system:** CMake ≥3.22, single `juce_add_gui_app` target
  (`CMakeLists.txt:26-34`). Sources are auto-discovered via
  `file(GLOB_RECURSE ... CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")`
  (`CMakeLists.txt:36-39`) — a new file under `Source/` never needs a
  CMakeLists.txt edit.
- **Plugin hosting:** VST3 and AU enabled, LV2 disabled
  (`CMakeLists.txt:53-55`); ASIO only compiles in when a local Steinberg ASIO
  SDK path is supplied, otherwise WASAPI exclusive mode is used on Windows
  (`CMakeLists.txt:62-68`).
- **Formats:** FLAC and OggVorbis enabled; no web browser component, no
  libcurl (`CMakeLists.txt:48-52`).
- **Persistence format:** `.ssproj`, plain JSON via `juce::JSON`/`juce::var`
  (`Source/Core/ProjectPersistence.cpp`) — no database, no external
  serialization library.
- **External runtime tools (both optional, user-supplied, never bundled):**
  a Demucs-compatible executable for stem separation, invoked as a
  subprocess and configured in Preferences (`Source/AI/StemSeparator.cpp`);
  a UTAU-compatible resampler executable for vocal synthesis, driven by
  parsed `oto.ini`/`.ust` files (`Source/Vocal/UtauRenderer.h`,
  `Source/IO/OtoIni.h`).
- **No bundled ML/AI models:** audio-to-MIDI transcription is an in-process
  DSP pipeline (onset detection + pYIN-style pitch tracking + note
  segmentation, `Source/AI/Transcriber.h:10-17`); MIDI generation
  (`Source/AI/Generator.h`) is a seeded, rule-and-corpus-driven arranger,
  not a trained model — both run entirely offline with no network calls.
- **Testing:** JUCE's built-in `juce::UnitTest` framework, run via
  `ScoreSmith.exe --run-tests`; results land in a fixed-name
  `test-results.txt` next to the executable (`Source/Main.cpp`) since the
  binary has no real stdout as a GUI-subsystem app.
- **Platforms:** Windows is the actively developed/tested target this
  session; the CMake setup also conditions for macOS (`CMakeLists.txt:6-8`)
  but that path is unexercised in this project's recent work.

None of this is new for Session view — it reuses the existing stack
end-to-end (same `juce_audio_*` modules for playback, same `.ssproj`/`juce::var`
path for persistence, same `juce::UnitTest` harness for tests). This section
exists for reference, not because Session view adds a dependency.

## Precedent already in the codebase

- `AudioEngine::previewMidiClip(TrackId, const MidiClip&)` / `stopPreview()`
  (`Source/Engine/AudioEngine.h:53-56`) already plays a single MIDI clip
  independent of `Transport`'s position, for the candidate gallery's instant
  preview. Its `Mixer::setPreviewClip` implementation
  (`Source/Mixer/Mixer.cpp:1549-1595`) uses a lock-free 4-slot ring buffer
  (`PreviewState previewSlots[numPreviewSlots]`, `previewSlot`/
  `previewGeneration` atomics) to hand a new clip from the message thread to
  the audio thread without a lock — **but there is only one active slot
  globally** (`activePreviewSlot` is a single int shared across the whole
  mixer, not one per track), and a preview plays once then clears itself
  (`Mixer.cpp:1449-1450`). Session view needs many tracks looping
  independently at once, so this pattern is generalized (see Engine section)
  rather than reused as-is.
- `Generator::Candidate` (`Source/AI/Generator.h:48-59`) already holds
  `std::vector<std::pair<Part, MidiClip>> parts` — a candidate is already
  shaped as "one MidiClip per part," which is exactly what a scene row of
  session clips needs.
- `GenerateView::adoptCandidate` (`Source/UI/GenerateView.cpp:550-578`)
  shows the established pattern for turning a candidate into project state:
  wrap the mutation in `performProjectEdit(project(), name, lambda)`
  (`Source/UI/UiSupport.cpp:81-87`) for undo/redo and dirty-flag handling.
  It creates one **new** `Track` per part every call — Session's "send
  candidates to grid" action needs different behavior (share one track per
  part across the whole batch, so scenes line up in the same column — see
  "Send to Session (GenerateView)" under UI, below).
- `Project::toVar()/loadFromVar()` (`Source/Core/Project.h:210-211`,
  implemented in `Source/Core/ProjectPersistence.cpp`) is the one
  persistence path (`.ssproj`, spec 10.4) everything project-shaped goes
  through. `performProjectEdit` itself works by snapshotting `toVar()`
  before and after a mutation (`Source/UI/UiSupport.cpp:20-56` region) — so
  any new state reachable from `Project::toVar()` gets undo/redo for free
  with zero new undo code.
- `TempoMap` (`Source/Core/Project.h:18-40`) already exposes
  `beatsToSeconds`, `secondsToBeats`, `barAndBeat`, and
  `timeSignatureAt` — everything needed to compute "seconds until the next
  bar boundary" without adding any new tempo/meter logic.

## Scope

**In scope (v1):**
- Session data model: `Scene`, `SessionClip` (MIDI or audio), per-track slots.
- Engine: per-track independent looping playback, fixed 1-bar-boundary
  quantized launch/replace/stop, a free-running clock independent of the
  main `Transport`'s play/stop state.
- UI: `SessionView` grid — track headers (read-only mirror), scene rows with
  a per-row launch/stop toggle, clip cells with click-to-launch and
  drag-and-drop.
- Dragging an existing `MidiClip` (from `TimelineView`/`PianoRollView`) onto
  a cell copies it in as a `SessionClip`.
- A new "Send to Session" action in `GenerateView` that turns one
  generation batch (1-8 candidates) into 1-8 new scenes, each candidate's
  per-part `MidiClip`s landing in the matching part's track column.
- Minimal scene management: add a scene, rename a scene, delete a scene.
- Persistence via `Project::toVar()/loadFromVar()`; undo via the existing
  `performProjectEdit` snapshot mechanism — no new undo code.

**Explicitly out of scope for v1 (deferred, do not build):**
- `UtauClip` session slots (no non-linear/no-position playback path exists
  for UTAU today — it renders to a cached file first; wiring that up is its
  own follow-up).
- Configurable quantize grid (1 bar is fixed and hard-coded; a settings UI
  for it is a follow-up if ever wanted).
- Scene reordering (rows are append-only in v1; drag-to-reorder is a
  follow-up, same category as the docking system's still-open
  layout-picker-UI follow-up).
- Editing a session clip's notes/audio in place (double-click to open in
  Piano Roll, etc.) — v1 clips are write-once-on-drop; re-recording means
  dragging a fresh clip onto the same cell to replace it.
- Floating/undocking the Session view specially — it is a normal dock panel
  like every other view, no special treatment needed.

## Data Model

New types in `Source/Core/Types.h`:

```cpp
using SceneId = juce::int64;
inline constexpr SceneId invalidSceneId = -1;

/** A launchable clip that lives in the Session grid, independent of the
    linear timeline - it has no startBeats.  Same field shapes as
    MidiClip/AudioClip minus the position, so conversion in both directions
    is a flat field copy. */
struct SessionClip
{
    enum class Kind { midi, audio };

    Kind         kind = Kind::midi;
    juce::String name;
    double       lengthBeats = 4.0;

    // kind == midi
    std::vector<Note> notes;          // relative to clip start, same convention as MidiClip::notes

    // kind == audio
    juce::File sourceFile;
    double     offsetSeconds  = 0.0;
    float      gainDb         = 0.0f;
    double     fadeInSec      = 0.0;
    double     fadeOutSec     = 0.0;
    bool       reversed       = false;
    double     playbackRate   = 1.0;
};
```

New type in `Source/Core/Project.h`, alongside `Bus`:

```cpp
/** One row of the Session grid (spec: Session view, Phase 3). */
struct Scene
{
    SceneId      id = invalidSceneId;
    juce::String name;
};
```

Additions to `Project` (`Source/Core/Project.h`):

```cpp
std::vector<Scene> scenes;                       // row order == vector order

Scene& addScene (const juce::String& name);
void   removeScene (SceneId);                    // also erases every track's slot for that id
Scene* findScene (SceneId) noexcept;

SceneId nextSceneId() noexcept { return ++lastSceneId; }
// new private member: SceneId lastSceneId = 0;
```

Additions to `Track` (`Source/Core/Project.h`):

```cpp
std::map<SceneId, SessionClip> sessionSlots;     // sparse - most cells are empty

SessionClip* findSessionClip (SceneId) noexcept;
void setSessionClip   (SceneId, SessionClip);
void clearSessionClip (SceneId);
```

`Project::removeTrack` already exists and simply erases the `Track`, which
takes its `sessionSlots` with it — no change needed there. `removeScene`
must walk every track and erase that `SceneId` key, mirroring how removing
something referenced elsewhere already has to fan out (see how
`Project::removeBus` re-points anything that fed the bus,
`Source/Core/Project.h:200-201`, for the established pattern of a removal
that must clean up references elsewhere).

## Engine

### Clock

New `Source/Engine/SessionClock.h/.cpp`, owned by `AudioEngine` alongside
`Transport transport;`:

```cpp
class SessionClock
{
public:
    void prepare (double sampleRate) noexcept;
    void advance (int numSamples) noexcept;   // called once per block, unconditionally - never gated on Transport::isPlaying()
    void reset() noexcept;                    // samplesElapsed = 0; called on project load

    /** Absolute sample count on this clock's own free-running timeline. */
    juce::int64 currentSample() const noexcept;

    /** Absolute sample (on this same timeline) of the next 1-bar boundary
        at or after currentSample(), using `tempo` to convert.  If currently
        sitting within ~1 sample of a boundary, returns currentSample() (no
        wait) rather than a full bar away. */
    juce::int64 nextBarBoundarySample (const TempoMap& tempo) const noexcept;

private:
    double sampleRate = 48000.0;
    std::atomic<juce::int64> samplesElapsed { 0 };   // audio thread writes, UI thread reads for boundary math
};
```

`nextBarBoundarySample` implementation (audio-thread-callable, read-only on
`tempo`):

```cpp
double elapsedSeconds = (double) currentSample() / sampleRate;
double elapsedBeats   = tempo.secondsToBeats (elapsedSeconds);
int bar; double beatInBar;
tempo.barAndBeat (elapsedBeats, bar, beatInBar);
auto ts = tempo.timeSignatureAt (elapsedBeats);
double beatsPerBar = ts.numerator * 4.0 / ts.denominator;
double beatsToBoundary = (beatInBar < 1.0e-6) ? 0.0 : (beatsPerBar - beatInBar);
double boundarySeconds = tempo.beatsToSeconds (elapsedBeats + beatsToBoundary);
return (juce::int64) std::llround (boundarySeconds * sampleRate);
```

The clock starts at `reset()` (project load / engine construction) and never
stops, matching the approved "independent of Transport" decision — clicking
a Session cell produces audio immediately-at-the-next-bar regardless of
whether the arrangement is playing.

### Per-track playback in `Mixer`

`Mixer::setPreviewClip`'s existing lock-free single-slot hand-off
(`Source/Mixer/Mixer.cpp:1549-1595`, state at `Mixer.cpp:525-573`) is the
pattern to generalize, not something Session reuses directly — the
implementer must read that real code before touching it (this project's
established practice; see how Task 10 of the docking-layout-system plan
required reading real JUCE source rather than trusting a description). The
per-track equivalent needs, per `TrackId`:

- The same lock-free double-buffered hand-off (message thread publishes a
  pending request + generation counter; audio thread notices the counter
  changed and adopts it) — but keyed per track instead of one global slot,
  and carrying a **target sample** (from `SessionClock::nextBarBoundarySample`)
  instead of applying immediately.
- Looping instead of one-shot: where `Mixer.cpp:1449-1450` clears
  `activePreviewSlot` once `previewPos > lengthSamples`, the session
  equivalent wraps (`posSamples %= lengthSamples`) and keeps playing.
- Two `SessionClip::Kind`s to render: `midi` (note on/off events scheduled
  against the loop position, same shape as `PreviewState::events` today)
  and `audio` (a resident `juce::AudioSampleBuffer` loaded on the message
  thread when the clip is placed in a cell, read with a wrapping cursor).

New public `Mixer` methods:

```cpp
void launchSessionClip (TrackId, const SessionClip&, juce::int64 launchAtSample);
void stopSessionClip   (TrackId, juce::int64 stopAtSample);
bool isSessionClipActive (TrackId) const noexcept;   // a clip is audibly looping right now
bool isSessionClipQueued (TrackId) const noexcept;   // a pending launch/stop is scheduled but hasn't reached its boundary sample yet
```

New public `AudioEngine` methods (thin wrappers that resolve the boundary
sample via `SessionClock` and the project's `TempoMap`, then forward to
`Mixer`):

```cpp
void launchSessionClip (TrackId, const SessionClip&);   // schedules for the next bar boundary
void stopSessionClip   (TrackId);                        // schedules stop for the next bar boundary
bool isSessionClipActive (TrackId) const noexcept;       // forwards to Mixer::isSessionClipActive
bool isSessionClipQueued (TrackId) const noexcept;       // forwards to Mixer::isSessionClipQueued
SessionClock& getSessionClock() noexcept;
```

`AudioEngine::audioDeviceIOCallbackWithContext` gains one new line calling
`sessionClock.advance (numSamples)` alongside the existing
`transport.advance (...)` — the clock always advances, independent of
`transport.isPlaying()`.

### Scene-wide firing

No new engine concept. `SessionView`'s scene-row toggle button, on press,
iterates that scene's filled slots across every track and calls
`AudioEngine::launchSessionClip` for each (or `stopSessionClip` for every
track that currently has that scene's row active, if toggling off) — same
per-track calls a single-cell click makes, just issued in a batch. Because
every call resolves against the *same* `SessionClock`, all of them land on
the identical bar boundary, giving synchronized scene-wide firing for free.

### Track-level exclusivity (candidate A/B switching)

Also no new engine concept, per the approved design: `launchSessionClip`
for a track that already has an active session voice simply publishes a new
pending request for that same track's slot — the existing "audio thread
adopts the newest generation at the boundary sample" mechanism already
means the new clip replaces the old one at that boundary. Firing two
candidate scenes on the same track back-to-back is just two ordinary
`launchSessionClip` calls.

## UI

New `Source/UI/SessionView.h/.cpp`, following the `GenerateView`/`ProjectView`
pattern (`Source/UI/UiSupport.h:149-167`): `class SessionView : public
ProjectView`, constructed as `session = std::make_unique<SessionView>
(ctx, uiState)` in `MainComponent::buildUi()`, replacing the current
`PlaceholderView` construction at `Source/UI/MainComponent.cpp:204-209`. No
other `buildUi()` wiring changes — `panelsById`/`displayNamesById` already
carry the `"session"`/`"Session"` keys.

Layout:
- One header cell per `Track` in `project().getTracks()` order: name,
  colour swatch, mute/solo state shown read-only (styled disabled/no-op on
  click, or simply non-interactive labels) — editing those stays in
  `MixerView`/`TimelineView` so there is exactly one place that owns each
  control, per this codebase's existing division of views by
  responsibility.
- One row per `Scene` in `project().scenes` order, with a launch/stop
  toggle button at the row's left edge (`Scene::name` as its label) plus a
  small "+" affordance below the last row to `performProjectEdit(...,
  "Add scene", [] { project().addScene(TRANS("Scene") + " " + juce::String(n)); })`.
  Right-click a scene name for a context menu with Rename (inline text
  editor) and Delete.
- Each (track, scene) intersection is a cell: empty state is a dashed
  outline (drop target only); filled state shows `SessionClip::name`, uses
  the owning `Track::colour` for its fill (matching how clips elsewhere
  take their track's colour rather than carrying their own), and shows a
  distinct visual state (e.g. a filled ring or brightened border) while
  `AudioEngine::isSessionClipActive` is true for that cell's track *and*
  that cell is the one most recently launched there; a cell that was just
  clicked but hasn't reached its scheduled boundary sample yet instead
  shows a distinct "queued" state driven by
  `AudioEngine::isSessionClipQueued`, matching how a real DAW indicates
  "will play at the next bar" vs. "playing now" — both booleans already
  come straight from `Mixer`, no new state needs to be tracked in the UI
  layer itself beyond "which cell did I most recently ask to launch for
  this track," kept locally so the view knows which cell to paint active
  once `isSessionClipActive` flips true.
- Click a filled cell: toggle launch/stop for that single cell (calls
  `AudioEngine::launchSessionClip`/`stopSessionClip` for its track).
  Click an empty cell: no-op (nothing to launch).
- Drag-and-drop target: a cell accepts a `juce::DragAndDropTarget` payload
  carrying an existing `MidiClip` — same `"ss.candidate"`-style mechanism
  `TimelineView::isInterestedInDragSource`/`itemDropped`
  (`Source/UI/TimelineView.cpp:2520-2554`) already consumes for candidate
  drags, generalized so `TimelineView`'s clip components can also start a
  drag carrying their own `MidiClip` (a new, narrowly-scoped drag-start
  path on the existing clip component, not a general "clips are now
  draggable everywhere" change). On drop, the dragged `MidiClip`'s fields
  (`notes`, `lengthBeats`, `name`) are copied into a new `SessionClip`
  (`kind = midi`) via `performProjectEdit(project(), TRANS("Add clip to
  session"), ...)` and written with `track.setSessionClip(sceneId, clip)`.

### "Send to Session" (GenerateView)

New button in `GenerateView` (alongside the existing per-candidate Adopt
button, but batch-scoped — operates on the whole current generation
result, not one candidate), calling a new `GenerateView::sendCandidatesToSession()`:

```cpp
void GenerateView::sendCandidatesToSession()
{
    if (ctx.project == nullptr || candidates.empty())
        return;

    performProjectEdit (project(), TRANS ("Send candidates to Session"), [this]
    {
        std::map<Generator::Part, Track*> partTracks;   // one track per part, shared across every candidate in this batch

        for (const auto& candidate : candidates)
        {
            auto& scene = project().addScene (candidate.name);

            for (const auto& [part, midiClip] : candidate.parts)
            {
                auto it = partTracks.find (part);
                Track* track = (it != partTracks.end())
                                 ? it->second
                                 : &project().addTrack (TrackType::midi, Generator::toString (part));
                partTracks[part] = track;

                SessionClip clip;
                clip.kind        = SessionClip::Kind::midi;
                clip.name        = midiClip.name.isNotEmpty() ? midiClip.name : Generator::toString (part);
                clip.lengthBeats = midiClip.lengthBeats;
                clip.notes       = midiClip.notes;
                track->setSessionClip (scene.id, clip);
            }
        }
    });

    ui.goTo (MainComponent::View::session);
}
```

This deliberately does **not** reuse `adoptCandidate`'s always-new-track
behavior (`GenerateView.cpp:565`) — that would put every candidate's bass
part on its own separate track, defeating the whole point of comparing
candidates via per-track exclusivity on a shared column. Building one
`partTracks` map local to a single batch send sidesteps any need to
identify "the existing bass track" by name across separate calls; it is
only ever used within the one action.

## Persistence

`Project::toVar()`/`loadFromVar()` (`Source/Core/ProjectPersistence.cpp`)
gain:
- A `"scenes"` array (mirroring the existing `"tracks"` array shape:
  `Source/Core/ProjectPersistence.cpp:498` for the write side,
  `:602` for the read side) — each entry `{ "id", "name" }`.
- Each track's existing per-track object (written where `"audioClips"`/
  `"midiClips"` are written today, `ProjectPersistence.cpp:459,474`) gains a
  `"sessionSlots"` array of `{ "sceneId", <SessionClip fields> }`.

No new undo code: because both additions are reachable from `toVar()`, the
existing `performProjectEdit`/`ProjectSnapshotAction` mechanism
(`Source/UI/UiSupport.cpp`, snapshotting `toVar()` before/after a lambda)
already captures and restores them for free, exactly like every other
project mutation in the codebase.

## Testing Strategy

- **Data model**: plain unit tests for `Project::addScene/removeScene`,
  `Track::setSessionClip/findSessionClip/clearSessionClip`, and a
  `toVar()` → `loadFromVar()` round-trip with scenes and slots populated —
  follow whatever existing test file already round-trips `Project`
  persistence (locate it during planning; do not invent a new one if one
  exists).
- **`SessionClock`**: pure math tests against `TempoMap` — boundary
  computation at various tempos/time signatures/mid-bar positions,
  including the "already on a boundary" zero-wait case.
- **`Mixer` per-track session playback**: mirror the existing
  preview-mechanism tests (locate them alongside `Mixer.cpp`'s other tests)
  — verify two different tracks can loop independently and simultaneously,
  verify a second `launchSessionClip` on the same track replaces the first
  only once the scheduled boundary sample is reached (not immediately),
  verify looping wraps rather than stopping.
- **`SessionView` UI logic**: follow `DockTests.cpp`'s established pattern
  in this codebase of driving handlers/overrides directly rather than
  simulating real mouse events (there is no GUI automation available in
  this environment for a native Win32 build) — click/drag handlers,
  scene add/rename/delete, drop handling.
- **`GenerateView::sendCandidatesToSession`**: verify N candidates produce
  N scenes, verify parts sharing a name across candidates land on the same
  track (the exact behavior the whole feature depends on), verify it does
  not disturb `adoptCandidate`'s existing per-candidate behavior.

## Risks / things the implementation plan must verify against real source

- The exact shape of `Mixer::Impl`'s existing per-track state struct (only
  its `id`/`soloed` fields were confirmed during design; whatever new
  per-track session-voice fields get added should live alongside it if a
  per-track struct already exists, rather than a second parallel
  `std::map<TrackId, ...>` — confirm during planning by reading
  `Mixer.cpp`'s `Impl` definition in full).
- Whether `TimelineView`'s clip components currently support starting a
  drag at all (only `GenerateView`'s `CandidateCard` was confirmed to call
  `startDragging`) — if not, the minimal drag-start addition described in
  the UI section needs its own small task rather than being assumed free.
