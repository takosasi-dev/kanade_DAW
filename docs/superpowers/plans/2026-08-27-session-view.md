# Session View (Phase 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `Session` tab's `PlaceholderView` with a real track x scene clip launcher — cells hold loopable MIDI/audio clips that fire independently of the linear timeline on a quantized 1-bar boundary, whole scene rows batch-fire, and a `Generator` run's candidates can be sent in as directly comparable scene rows.

**Architecture:** A new `SessionClip`/`Scene` data model lives alongside (not inside) the existing timeline `Track` clips, persisted through the existing `.ssproj` `toVar()`/`loadFromVar()` path with zero new undo code (the existing whole-project-snapshot `performProjectEdit` already covers it). Playback is driven by a new free-running `SessionClock` (independent of `Transport`'s play/stop state) and a per-track lock-free request ring generalized from `Mixer`'s existing single-slot preview mechanism. `SessionView` is a new `ProjectView`, wired into `MainComponent::buildUi()` in place of the placeholder.

**Tech Stack:** JUCE 8.0.6, C++20, existing `ss` namespace conventions.

**Spec:** `docs/superpowers/specs/2026-08-27-session-view-design.md`

## Global Constraints

- No `UtauClip` session slots in v1 — `SessionClip::Kind` is `midi` or `audio` only.
- Launch quantization is fixed at 1 bar; there is no settings UI for it and no other grid size.
- `SessionClip` has no `startBeats` — it is entirely independent of the linear timeline.
- The session playback clock never stops or waits on `Transport::isPlaying()` — it free-runs from the moment the engine starts / a project loads.
- Exactly one session clip can be active per track at a time; launching a second one on the same track replaces the first at the next bar boundary (no crossfade — a hard cut is accepted for v1).
- A launched clip loops until stopped or replaced — never a one-shot.
- All new persisted fields are additive to the existing `.ssproj` JSON — do not bump `projectFormatVersion`, and every read uses `getProperty(key, default)` so older files still load.
- New `.cpp`/`.h` files under `Source/` are auto-discovered by CMake's `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` in `CMakeLists.txt` — never edit `CMakeLists.txt` to register a new source file.
- Match existing formatting exactly: Allman braces, `ss` namespace, `noexcept` on pure accessors, doc comments only where they explain a non-obvious *why* (see any existing header for the house style) — do not add narrative comments explaining *what* code does.
- Every new `juce::UnitTest` subclass is registered with a single `static <ClassName> <instanceName>;` line inside `namespace ss { ... }`, immediately after the class — this is how every existing test file in this codebase registers itself; a class without that line silently never runs.
- Deliberate scope cuts get a `ponytail:` comment naming the limitation and what would lift it, per this project's established convention this session.

---

### Task 1: Session data model (`Scene`, `SessionClip`, `Project`/`Track` storage)

**Context:** This is pure data — no engine, no UI, no persistence yet (Task 2). It only needs to compile and hold values correctly.

**Files:**
- Modify: `Source/Core/Types.h`
- Modify: `Source/Core/Project.h`
- Modify: `Source/Core/Project.cpp`
- Create: `Source/Core/ProjectTests.cpp`

**Interfaces:**
- Produces: `ss::SceneId`, `ss::invalidSceneId`, `ss::SessionClip` (fields: `kind`, `name`, `lengthBeats`, `notes`, `sourceFile`, `offsetSeconds`, `gainDb`, `fadeInSec`, `fadeOutSec`, `reversed`, `playbackRate`), `ss::Scene` (fields: `id`, `name`), `Project::scenes`, `Project::addScene(const juce::String&) -> Scene&`, `Project::removeScene(SceneId)`, `Project::findScene(SceneId) noexcept -> Scene*`, `Track::sessionSlots`, `Track::findSessionClip(SceneId) noexcept -> SessionClip*`, `Track::setSessionClip(SceneId, SessionClip)`, `Track::clearSessionClip(SceneId)`.
- Consumes: nothing new — `Note` (`Source/Core/Types.h`), `TrackId`/`ClipId` numeric-typedef convention already in `Types.h`, `Project::markDirty()` (`Source/Core/Project.h:220`).

- [ ] **Step 1: Write the failing tests**

Create `Source/Core/ProjectTests.cpp`:

```cpp
#include "Core/Project.h"
#include <juce_core/juce_core.h>

namespace ss
{

class ProjectSessionUnitTests final : public juce::UnitTest
{
public:
    ProjectSessionUnitTests() : juce::UnitTest ("Session data model", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("addScene assigns increasing ids and keeps insertion order")
        {
            Project project;
            auto& a = project.addScene ("Intro");
            auto& b = project.addScene ("Verse");

            expect (a.id != invalidSceneId);
            expect (b.id != a.id);
            expectEquals ((int) project.scenes.size(), 2);
            expectEquals (project.scenes[0].name, juce::String ("Intro"));
            expectEquals (project.scenes[1].name, juce::String ("Verse"));
        }

        beginTest ("findScene returns nullptr for an id that was never added")
        {
            Project project;
            expect (project.findScene ((SceneId) 999) == nullptr);
        }

        beginTest ("removeScene erases the scene and every track's slot for it")
        {
            Project project;
            auto& scene = project.addScene ("Verse");
            auto& track = project.addTrack (TrackType::midi, "Lead");

            SessionClip clip;
            clip.kind = SessionClip::Kind::midi;
            clip.name = "Riff";
            track.setSessionClip (scene.id, clip);

            expect (track.findSessionClip (scene.id) != nullptr);

            project.removeScene (scene.id);

            expect (project.findScene (scene.id) == nullptr);
            expect (track.findSessionClip (scene.id) == nullptr);
        }

        beginTest ("removeScene on an unknown id is a harmless no-op")
        {
            Project project;
            project.addScene ("Verse");
            project.removeScene ((SceneId) 999);
            expectEquals ((int) project.scenes.size(), 1);
        }

        beginTest ("Track::setSessionClip / findSessionClip / clearSessionClip round-trip")
        {
            Track track (1, TrackType::midi, "Bass");

            expect (track.findSessionClip ((SceneId) 1) == nullptr);

            SessionClip clip;
            clip.kind        = SessionClip::Kind::midi;
            clip.name        = "Bass loop";
            clip.lengthBeats = 8.0;
            clip.notes.push_back ({ 36, 0.0, 1.0, 100, 1, 1.0f });

            track.setSessionClip ((SceneId) 1, clip);

            auto* found = track.findSessionClip ((SceneId) 1);
            expect (found != nullptr);
            expectEquals (found->name, juce::String ("Bass loop"));
            expectWithinAbsoluteError (found->lengthBeats, 8.0, 1.0e-9);
            expectEquals ((int) found->notes.size(), 1);

            track.clearSessionClip ((SceneId) 1);
            expect (track.findSessionClip ((SceneId) 1) == nullptr);
        }

        beginTest ("SessionClip defaults to an empty midi-kind clip")
        {
            SessionClip clip;
            expect (clip.kind == SessionClip::Kind::midi);
            expectWithinAbsoluteError (clip.lengthBeats, 4.0, 1.0e-9);
            expectWithinAbsoluteError ((double) clip.playbackRate, 1.0, 1.0e-9);
            expect (! clip.reversed);
            expect (clip.notes.empty());
        }
    }
};

static ProjectSessionUnitTests projectSessionUnitTests;

}
```

- [ ] **Step 2: Run tests to verify they fail**

Build the test target (same build command Task 1 of the docking-layout-system plan used — `cmake --build` against the `E:\MIDIDAW` junction; check `.superpowers/sdd/2026-08-26-docking-layout-system/` briefs if the exact command needs re-confirming) and run `ScoreSmith.exe --run-tests`, then read the fixed-name `test-results.txt` next to the executable (this binary has no stdout — see `Source/Main.cpp`'s `runUnitTests`).
Expected: **compile failure** — `SceneId`, `SessionClip`, `Project::addScene`, `Track::setSessionClip` etc. do not exist yet. A compile failure is the correct "red" state here since Step 1 adds tests against types Step 3 hasn't created.

- [ ] **Step 3: Implement**

In `Source/Core/Types.h`, add after the `invalidClipId` declarations:

```cpp
    using SceneId = juce::int64;
    inline constexpr SceneId invalidSceneId = -1;
```

Add after the `Note` struct (before `TempoEvent`):

```cpp
    /** A launchable clip in the Session grid (spec: Session view, Phase 3).
        No startBeats - it is independent of the linear timeline.  Field
        shapes mirror MidiClip/AudioClip minus the position, so conversion
        either direction is a flat field copy. */
    struct SessionClip
    {
        enum class Kind { midi, audio };

        Kind         kind = Kind::midi;
        juce::String name;
        double       lengthBeats = 4.0;

        // kind == midi
        std::vector<Note> notes;      // relative to clip start, same convention as MidiClip::notes

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

`SessionClip` needs `juce::File`, already available via `<juce_core/juce_core.h>` which `Types.h` already includes.

In `Source/Core/Project.h`, add after the `Bus` struct (before `class Track`):

```cpp
    /** One row of the Session grid (spec: Session view, Phase 3). */
    struct Scene
    {
        SceneId      id = invalidSceneId;
        juce::String name;
    };
```

In `class Track`, add alongside the existing clip vectors:

```cpp
        std::map<SceneId, SessionClip> sessionSlots;     // sparse - most cells are empty
```

(`Project.h` needs `#include <map>` — check the top of the file; if it is not already included by one of the existing `juce_*` headers, add `#include <map>` next to the existing `#include <memory>`.)

Add alongside `findAudioClip`/`findMidiClip`/`findUtauClip`:

```cpp
        SessionClip* findSessionClip (SceneId) noexcept;
        void setSessionClip   (SceneId, SessionClip);
        void clearSessionClip (SceneId);
```

In `class Project`, add alongside the track accessors:

```cpp
        // --- scenes (Session view, Phase 3) ----------------------------------
        std::vector<Scene> scenes;                       // row order == vector order

        Scene& addScene (const juce::String& name);
        /** Also erases every track's slot for this id - a scene reference left
            dangling on a track would show as a phantom cell. */
        void   removeScene (SceneId);
        Scene* findScene (SceneId) noexcept;

        SceneId nextSceneId() noexcept { return ++lastSceneId; }
```

Add a private member alongside `lastClipId`/`lastBusId` (find them near the bottom of the `private:` section):

```cpp
        SceneId lastSceneId = 0;
```

In `Source/Core/Project.cpp`, add near `Track::findMidiClip`:

```cpp
    SessionClip* Track::findSessionClip (SceneId sceneId) noexcept
    {
        auto it = sessionSlots.find (sceneId);
        return it != sessionSlots.end() ? &it->second : nullptr;
    }

    void Track::setSessionClip (SceneId sceneId, SessionClip clip)
    {
        sessionSlots[sceneId] = std::move (clip);
    }

    void Track::clearSessionClip (SceneId sceneId)
    {
        sessionSlots.erase (sceneId);
    }
```

Add near `Project::addBus`/`removeBus`/`findBus`:

```cpp
    Scene& Project::addScene (const juce::String& sceneName)
    {
        Scene scene;
        scene.id   = nextSceneId();
        scene.name = sceneName.isNotEmpty() ? sceneName : ("Scene " + juce::String (scene.id));

        scenes.push_back (std::move (scene));
        markDirty();
        return scenes.back();
    }

    void Project::removeScene (SceneId sceneId)
    {
        const auto before = scenes.size();

        scenes.erase (std::remove_if (scenes.begin(), scenes.end(),
                                      [sceneId] (const Scene& s) { return s.id == sceneId; }),
                      scenes.end());

        if (scenes.size() == before)
            return;

        for (auto& t : tracks)
            t->clearSessionClip (sceneId);

        markDirty();
    }

    Scene* Project::findScene (SceneId sceneId) noexcept
    {
        for (auto& s : scenes)
            if (s.id == sceneId)
                return &s;

        return nullptr;
    }
```

- [ ] **Step 4: Run tests to verify they pass**

Same build+run as Step 2. Expected: all `ProjectSessionUnitTests` assertions pass, 0 failed, and every pre-existing test still passes (no regression from the `Track`/`Project` header changes).

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/Types.h Source/Core/Project.h Source/Core/Project.cpp Source/Core/ProjectTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add Session view data model: Scene, SessionClip, per-track slots"
```

---

### Task 2: Session persistence (`.ssproj` round-trip)

**Files:**
- Modify: `Source/Core/ProjectPersistence.cpp`
- Test: `Source/Core/ProjectTests.cpp` (extend)

**Interfaces:**
- Consumes: `Project::scenes`, `Project::lastSceneId` (Task 1 — the private member is accessible here because `toVar`/`loadFromVar` are themselves `Project` methods), `Track::sessionSlots`, `notesToVar`/`notesFromVar` (`Source/Core/ProjectPersistence.cpp:35-`, existing), `utauClipsToVar`/`utauClipsFromVar` (`ProjectPersistence.cpp:109-`, existing — the pattern this task's new helpers mirror for the same reason: a per-clip file path needing project-relative resolution).
- Produces: `sessionSlotsToVar`/`sessionSlotsFromVar` (file-local helpers in the anonymous namespace at the top of `ProjectPersistence.cpp`) — not used by any other task, but named in case a future follow-up needs them.

- [ ] **Step 1: Write the failing test**

Extend `Source/Core/ProjectTests.cpp`, adding this `beginTest` block to `ProjectSessionUnitTests::runTest()`:

```cpp
        beginTest ("scenes and session slots round-trip through toVar/loadFromVar")
        {
            Project project;
            auto& scene1 = project.addScene ("Intro");
            auto& scene2 = project.addScene ("Drop");
            auto& track  = project.addTrack (TrackType::midi, "Lead");

            SessionClip midiClip;
            midiClip.kind        = SessionClip::Kind::midi;
            midiClip.name        = "Lead riff";
            midiClip.lengthBeats = 8.0;
            midiClip.notes.push_back ({ 64, 0.0, 2.0, 110, 1, 1.0f });
            track.setSessionClip (scene1.id, midiClip);

            SessionClip audioClip;
            audioClip.kind          = SessionClip::Kind::audio;
            audioClip.name          = "Vocal chop";
            audioClip.lengthBeats   = 4.0;
            audioClip.sourceFile    = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                          .getChildFile ("chop.wav");
            audioClip.offsetSeconds = 0.5;
            audioClip.gainDb        = -2.5f;
            audioClip.reversed      = true;
            audioClip.playbackRate  = 1.25;
            track.setSessionClip (scene2.id, audioClip);

            const auto scene1Id = scene1.id;
            const auto scene2Id = scene2.id;
            const auto trackId  = track.getId();

            Project reloaded;
            expect (reloaded.loadFromVar (project.toVar()));

            expectEquals ((int) reloaded.scenes.size(), 2);
            expectEquals (reloaded.scenes[0].name, juce::String ("Intro"));
            expectEquals (reloaded.scenes[1].name, juce::String ("Drop"));

            auto* reloadedTrack = reloaded.findTrack (trackId);
            expect (reloadedTrack != nullptr);

            if (reloadedTrack != nullptr)
            {
                auto* m = reloadedTrack->findSessionClip (scene1Id);
                expect (m != nullptr);
                if (m != nullptr)
                {
                    expect (m->kind == SessionClip::Kind::midi);
                    expectEquals (m->name, juce::String ("Lead riff"));
                    expectWithinAbsoluteError (m->lengthBeats, 8.0, 1.0e-9);
                    expectEquals ((int) m->notes.size(), 1);
                    expectEquals (m->notes[0].pitch, 64);
                }

                auto* a = reloadedTrack->findSessionClip (scene2Id);
                expect (a != nullptr);
                if (a != nullptr)
                {
                    expect (a->kind == SessionClip::Kind::audio);
                    expectEquals (a->name, juce::String ("Vocal chop"));
                    expectWithinAbsoluteError (a->offsetSeconds, 0.5, 1.0e-9);
                    expectWithinAbsoluteError ((double) a->gainDb, -2.5, 1.0e-4);
                    expect (a->reversed);
                    expectWithinAbsoluteError (a->playbackRate, 1.25, 1.0e-9);
                    expectEquals (a->sourceFile.getFileName(), juce::String ("chop.wav"));
                }
            }

            // A new scene added after reload must not collide with the ids that came from disk.
            auto& scene3 = reloaded.addScene ("Outro");
            expect (scene3.id != scene1Id && scene3.id != scene2Id);
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build+run as Task 1. Expected: fails — `reloaded.scenes` is empty and `findSessionClip` returns `nullptr`, since nothing writes/reads these fields yet.

- [ ] **Step 3: Implement**

In `Source/Core/ProjectPersistence.cpp`, add to the anonymous namespace, near `utauClipsToVar`/`utauClipsFromVar`:

```cpp
        juce::var sessionSlotsToVar (const std::map<SceneId, SessionClip>& slots, const juce::File& projectFile)
        {
            juce::Array<juce::var> array;

            for (const auto& [sceneId, clip] : slots)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("sceneId", (juce::int64) sceneId);
                obj->setProperty ("kind", clip.kind == SessionClip::Kind::audio ? "audio" : "midi");
                obj->setProperty ("name", clip.name);
                obj->setProperty ("length", clip.lengthBeats);
                obj->setProperty ("notes", notesToVar (clip.notes));
                obj->setProperty ("file", clip.sourceFile.getFullPathName().isNotEmpty() && projectFile.existsAsFile()
                                              ? clip.sourceFile.getRelativePathFrom (projectFile.getParentDirectory())
                                              : clip.sourceFile.getFullPathName());
                obj->setProperty ("offset", clip.offsetSeconds);
                obj->setProperty ("gainDb", clip.gainDb);
                obj->setProperty ("fadeIn", clip.fadeInSec);
                obj->setProperty ("fadeOut", clip.fadeOutSec);
                obj->setProperty ("reversed", clip.reversed);
                obj->setProperty ("rate", clip.playbackRate);
                array.add (juce::var (obj));
            }

            return array;
        }

        std::map<SceneId, SessionClip> sessionSlotsFromVar (const juce::var& v, const juce::File& projectFile)
        {
            std::map<SceneId, SessionClip> slots;

            if (auto* array = v.getArray())
            {
                for (const auto& item : *array)
                {
                    const auto sceneId = (SceneId) (juce::int64) item.getProperty ("sceneId", 0);

                    SessionClip clip;
                    clip.kind        = item.getProperty ("kind", "midi").toString() == "audio"
                                           ? SessionClip::Kind::audio : SessionClip::Kind::midi;
                    clip.name        = item.getProperty ("name", {}).toString();
                    clip.lengthBeats = (double) item.getProperty ("length", 4.0);
                    clip.notes       = notesFromVar (item.getProperty ("notes", {}));

                    const auto path = item.getProperty ("file", {}).toString();
                    if (path.isNotEmpty())
                        clip.sourceFile = juce::File::isAbsolutePath (path)
                                              ? juce::File (path)
                                              : projectFile.getParentDirectory().getChildFile (path);

                    clip.offsetSeconds = (double) item.getProperty ("offset", 0.0);
                    clip.gainDb        = (float) (double) item.getProperty ("gainDb", 0.0);
                    clip.fadeInSec     = (double) item.getProperty ("fadeIn", 0.0);
                    clip.fadeOutSec    = (double) item.getProperty ("fadeOut", 0.0);
                    clip.reversed      = (bool) item.getProperty ("reversed", false);
                    clip.playbackRate  = (double) item.getProperty ("rate", 1.0);

                    slots[sceneId] = std::move (clip);
                }
            }

            return slots;
        }
```

In `Project::toVar()`, add right after the `markers` block (the block that ends `root->setProperty ("markers", array);`) and before the `tracks` block:

```cpp
        {
            juce::Array<juce::var> array;

            for (const auto& s : scenes)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id",   (juce::int64) s.id);
                obj->setProperty ("name", s.name);
                array.add (juce::var (obj));
            }

            root->setProperty ("scenes", array);
        }

        root->setProperty ("lastSceneId", (juce::int64) lastSceneId);
```

In the same function's per-track loop, add right after `obj->setProperty ("utauClips", utauClipsToVar (t->utauClips, file));`:

```cpp
                obj->setProperty ("sessionSlots", sessionSlotsToVar (t->sessionSlots, file));
```

In `Project::loadFromVar()`, add alongside the existing `lastTrackId`/`lastClipId`/`lastBusId` reads (the three lines reading `root.getProperty ("lastTrackId"/"lastClipId"/"lastBusId", ...)`):

```cpp
        lastSceneId = (SceneId) (juce::int64) root.getProperty ("lastSceneId", 0);
```

Add right after that block, before the `tracks` array is read:

```cpp
        if (auto* sceneArray = root.getProperty ("scenes", {}).getArray())
        {
            for (const auto& item : *sceneArray)
            {
                Scene scene;
                scene.id   = (SceneId) (juce::int64) item.getProperty ("id", 0);
                scene.name = item.getProperty ("name", {}).toString();

                lastSceneId = juce::jmax (lastSceneId, scene.id);
                scenes.push_back (std::move (scene));
            }
        }
```

(This defensive re-max against actually-seen ids mirrors exactly how `lastTrackId`/`lastClipId`/`lastBusId` are each re-maxed while reading their own collections — see the existing `lastTrackId = juce::jmax (lastTrackId, id);` line in the tracks loop.)

In the per-track read loop, add right after `track->utauClips = utauClipsFromVar (item.getProperty ("utauClips", {}), file);`:

```cpp
                track->sessionSlots = sessionSlotsFromVar (item.getProperty ("sessionSlots", {}), file);
```

No explicit `scenes.clear()` is needed: `loadFromVar` is only ever called on a freshly-constructed `Project` (confirmed by its own callers — `Project::loadFrom(juce::File)`'s pattern and the existing round-trip test both do `Project reloaded; reloaded.loadFromVar(...)`), and neither `tracks` nor `buses` are cleared at the top of `loadFromVar` either — `scenes` should match that existing convention exactly, not diverge from it.

- [ ] **Step 4: Run tests to verify they pass**

Same build+run. Expected: the new test passes, 0 failed overall.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/ProjectPersistence.cpp Source/Core/ProjectTests.cpp
git -C "E:/MIDI&DAW" commit -m "Persist Session scenes and per-track session slots in .ssproj"
```

---

### Task 3: `SessionClock`

**Files:**
- Create: `Source/Engine/SessionClock.h`
- Create: `Source/Engine/SessionClock.cpp`
- Test: `Source/Engine/EngineTests.cpp` (extend)

**Interfaces:**
- Consumes: `ss::TempoMap` (`Source/Core/Project.h:18-40`) — `secondsToBeats`, `barAndBeat`, `timeSignatureAt`, `beatsToSeconds`, all already `const noexcept`.
- Produces: `ss::SessionClock` — `prepare(double sampleRate) noexcept`, `advance(int numSamples) noexcept`, `reset() noexcept`, `currentSample() const noexcept -> juce::int64`, `nextBarBoundarySample(const TempoMap&) const noexcept -> juce::int64`. Task 4 (Mixer/AudioEngine) is the consumer.

- [ ] **Step 1: Write the failing tests**

Create `Source/Engine/SessionClock.h`:

```cpp
#pragma once
#include "Core/Project.h"
#include <atomic>

namespace ss
{
    /** Free-running sample clock for Session view launches (spec: Session
        view, Phase 3).  Deliberately independent of Transport - it never
        stops or waits on Transport::isPlaying(), so a Session cell can be
        clicked and heard even while the arrangement is stopped.  It shares
        only the project's TempoMap, passed in per call rather than held, so
        it stays trivially testable without a Project. */
    class SessionClock
    {
    public:
        void prepare (double sampleRateToUse) noexcept { sampleRate = sampleRateToUse > 0.0 ? sampleRateToUse : 48000.0; }

        /** Called once per audio block, unconditionally - never gated on
            Transport's play/stop state. */
        void advance (int numSamples) noexcept { samplesElapsed.fetch_add (numSamples, std::memory_order_release); }

        /** Called when a project loads, so bar-boundary math starts fresh. */
        void reset() noexcept { samplesElapsed.store (0, std::memory_order_release); }

        juce::int64 currentSample() const noexcept { return samplesElapsed.load (std::memory_order_acquire); }

        /** Absolute sample (on this clock's own timeline) of the next 1-bar
            boundary at or after currentSample().  Returns currentSample()
            itself (no wait) if already within a sample of a boundary. */
        juce::int64 nextBarBoundarySample (const TempoMap& tempo) const noexcept;

    private:
        double sampleRate = 48000.0;
        std::atomic<juce::int64> samplesElapsed { 0 };
    };
}
```

Create `Source/Engine/SessionClock.cpp`:

```cpp
#include "Engine/SessionClock.h"

namespace ss
{
    juce::int64 SessionClock::nextBarBoundarySample (const TempoMap& tempo) const noexcept
    {
        const auto elapsedSeconds = (double) currentSample() / sampleRate;
        const auto elapsedBeats   = tempo.secondsToBeats (elapsedSeconds);

        int bar = 0;
        double beatInBar = 0.0;
        tempo.barAndBeat (elapsedBeats, bar, beatInBar);

        const auto ts = tempo.timeSignatureAt (elapsedBeats);
        const auto beatsPerBar = ts.numerator * 4.0 / ts.denominator;
        const auto beatsToBoundary = (beatInBar < 1.0e-6) ? 0.0 : (beatsPerBar - beatInBar);

        const auto boundarySeconds = tempo.beatsToSeconds (elapsedBeats + beatsToBoundary);
        return (juce::int64) std::llround (boundarySeconds * sampleRate);
    }
}
```

Add to `Source/Engine/EngineTests.cpp` a new test class (mirroring how `TransportTests`/`RecorderTests` are each their own class in this same file):

```cpp
#include "Engine/SessionClock.h"

// ... alongside the existing includes at the top of the file

class SessionClockTests final : public juce::UnitTest
{
public:
    SessionClockTests() : juce::UnitTest ("SessionClock", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("nextBarBoundarySample returns the current sample when already on a boundary")
        {
            TempoMap tempo;
            tempo.setEvents ({ { 0.0, 120.0 } });
            tempo.setTimeSignatures ({ { 0.0, 4, 4 } });

            SessionClock clock;
            clock.prepare (48000.0);

            expectEquals (clock.nextBarBoundarySample (tempo), (juce::int64) 0);
        }

        beginTest ("nextBarBoundarySample computes the next 1-bar boundary at 120bpm 4/4")
        {
            TempoMap tempo;
            tempo.setEvents ({ { 0.0, 120.0 } });
            tempo.setTimeSignatures ({ { 0.0, 4, 4 } });

            SessionClock clock;
            clock.prepare (48000.0);

            // 120bpm -> 0.5s/beat, 4/4 -> 4 beats/bar -> 2s/bar -> 96000 samples/bar.
            clock.advance (48000);   // 1s in = halfway through the first bar

            expectEquals (clock.nextBarBoundarySample (tempo), (juce::int64) 96000);
        }

        beginTest ("nextBarBoundarySample respects a 3/4 time signature")
        {
            TempoMap tempo;
            tempo.setEvents ({ { 0.0, 120.0 } });
            tempo.setTimeSignatures ({ { 0.0, 3, 4 } });

            SessionClock clock;
            clock.prepare (48000.0);

            // 3/4 at 120bpm -> 3 beats/bar -> 1.5s/bar -> 72000 samples/bar.
            clock.advance (36000);   // halfway through the first bar

            expectEquals (clock.nextBarBoundarySample (tempo), (juce::int64) 72000);
        }

        beginTest ("advance accumulates and reset returns to zero, independent of any transport")
        {
            SessionClock clock;
            clock.prepare (48000.0);

            clock.advance (100);
            clock.advance (200);
            expectEquals (clock.currentSample(), (juce::int64) 300);

            clock.reset();
            expectEquals (clock.currentSample(), (juce::int64) 0);
        }
    }
};

static SessionClockTests sessionClockTests;
```

Place this class in the same `namespace ss { ... }` block the existing `TransportTests`/`RecorderTests` live in, with its own `static SessionClockTests sessionClockTests;` line, matching how `RecorderTests` gets its own registration line separate from `TransportTests`'s.

- [ ] **Step 2: Run tests to verify they fail**

Same build+run as Task 1. Expected: compile failure (`SessionClock` does not exist) until Step 3's files are added — since Step 1 already created the real header/cpp above, expect instead a link or "file not found" style failure only if you write the test before creating the header; if you created `SessionClock.h/.cpp` in this same step as shown, this Step 2 should instead show the test **passing already** for the math (the header above is a complete implementation, not a stub). Either way, run the suite now and confirm the new assertions pass and nothing else regressed — treat this as the "verify" step regardless of whether anything was red first.

- [ ] **Step 3: (implementation already written above in Step 1 — nothing further to do)**

- [ ] **Step 4: Run tests to verify they pass**

Confirm all `SessionClockTests` assertions pass and the full suite total grew by exactly these 4 tests' assertions with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Engine/SessionClock.h Source/Engine/SessionClock.cpp Source/Engine/EngineTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add SessionClock: free-running clock for Session view bar-boundary launches"
```

---

### Task 4: Per-track session playback in `Mixer`, wired into `AudioEngine`

**Context:** This is the highest-risk task in the plan — real-time audio-thread code. `Mixer::setPreviewClip`'s existing lock-free single-slot hand-off (`Source/Mixer/Mixer.cpp:1549-1595`, its state at `:525-573`, its audio-thread consumption inside `Mixer::process()` at `:1339-1451`) is the pattern this generalizes — **read all of it yourself in the real file before writing anything**; line numbers here are a starting point, not a promise they haven't drifted. The critical real-time-safety rule this project already follows: nothing that touches the audio thread may allocate or block. That means every `SessionClip` must be fully converted into ready-to-play data (a sorted `TimedMidi` event list, or a loaded `juce::AudioSampleBuffer`) on the **message thread**, inside the new `launchSessionClip` call, before it is ever handed to the audio thread — exactly how `setPreviewClip` already builds `preview.events` before publishing the slot index. The audio thread only ever swaps which pre-built slot it reads from; it never builds one.

**Files:**
- Modify: `Source/Mixer/Mixer.h`
- Modify: `Source/Mixer/Mixer.cpp`
- Modify: `Source/Engine/AudioEngine.h`
- Modify: `Source/Engine/AudioEngine.cpp`
- Test: `Source/Mixer/MixerTests.cpp` (extend)

**Interfaces:**
- Consumes: `SessionClock` (Task 3), `SessionClip`/`Scene`/`Track::sessionSlots` (Task 1), `TimedMidi` (`Source/Mixer/Mixer.cpp:454-458`, existing, private to `Mixer.cpp`), `Mixer::Impl::TrackState` (`Mixer.cpp:503-523`, existing), `Mixer::process(...)` (`Source/Mixer/Mixer.h:85-87`, existing public synchronous entry point — this is what the tests below drive directly; it needs no real audio device).
- Produces: `Mixer::launchSessionClip(TrackId, const SessionClip&, juce::int64 launchAtSample)`, `Mixer::stopSessionClip(TrackId, juce::int64 stopAtSample)`, `Mixer::isSessionClipActive(TrackId) const noexcept -> bool`, `Mixer::isSessionClipQueued(TrackId) const noexcept -> bool`; `AudioEngine::launchSessionClip(TrackId, const SessionClip&)`, `AudioEngine::stopSessionClip(TrackId)`, `AudioEngine::isSessionClipActive(TrackId) const noexcept`, `AudioEngine::isSessionClipQueued(TrackId) const noexcept`, `AudioEngine::getSessionClock() noexcept -> SessionClock&`. Tasks 5-8 (UI) consume the `AudioEngine` methods only; nothing outside `Mixer.cpp` touches `TrackState`.

- [ ] **Step 1: Write the failing tests**

Add to `Source/Mixer/MixerTests.cpp`. This drives `Mixer::process()` directly with synthetic buffers — no audio device, no plugin, matching this file's own established scope ("only the parts that can be checked without an audio device or a plugin"); it is possible here because `SessionClip::Kind::audio` playback is a raw buffer copy with no instrument involved, which makes it fully checkable by inspecting `process()`'s output samples.

```cpp
        beginTest ("launchSessionClip on an audio-kind clip plays only after its boundary sample, then loops")
        {
            PluginManager pm;
            Mixer mixer (pm);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            // Deliberately no sourceFile: Step 3's launchSessionClip treats a
            // clip whose sourceFile does not exist as a valid zero-length
            // buffer (silent, lengthSamples == 0) rather than an error, so
            // isSessionClipActive still flips true - "active" reflects
            // scheduling state, not audio content. This keeps the test free
            // of real file I/O.

            const juce::int64 boundarySample = 1000;
            mixer.launchSessionClip (trackId, clip, boundarySample);

            expect (mixer.isSessionClipQueued (trackId));
            expect (! mixer.isSessionClipActive (trackId));

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            // Block 1: samples [0, 512) - entirely before the boundary at 1000.
            mixer.process (out, 0, midi, noInput, true);
            expect (! mixer.isSessionClipActive (trackId));

            // Block 2: samples [512, 1024) - still before 1000.
            mixer.process (out, 512, midi, noInput, true);
            expect (! mixer.isSessionClipActive (trackId));

            // Block 3: samples [1024, 1536) - crosses the boundary at 1000.
            mixer.process (out, 1024, midi, noInput, true);
            expect (mixer.isSessionClipActive (trackId));
            expect (! mixer.isSessionClipQueued (trackId));
        }

        beginTest ("stopSessionClip stops playback only once its boundary sample is reached")
        {
            PluginManager pm;
            Mixer mixer (pm);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, clip, 0);   // active immediately (boundary already at sample 0)

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;
            mixer.process (out, 0, midi, noInput, true);
            expect (mixer.isSessionClipActive (trackId));

            mixer.stopSessionClip (trackId, 2000);
            expect (mixer.isSessionClipQueued (trackId));

            mixer.process (out, 512, midi, noInput, true);    // [512, 1024) - before 2000
            expect (mixer.isSessionClipActive (trackId));

            mixer.process (out, 1536, midi, noInput, true);   // [1536, 2048) - crosses 2000
            expect (! mixer.isSessionClipActive (trackId));
        }

        beginTest ("launching a second session clip on the same track replaces the first at the new boundary")
        {
            PluginManager pm;
            Mixer mixer (pm);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip first;
            first.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, first, 0);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;
            mixer.process (out, 0, midi, noInput, true);
            expect (mixer.isSessionClipActive (trackId));

            SessionClip second;
            second.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, second, 2000);

            expect (mixer.isSessionClipActive (trackId));   // the first clip keeps playing until the boundary
            expect (mixer.isSessionClipQueued (trackId));

            mixer.process (out, 1536, midi, noInput, true);  // crosses 2000
            expect (mixer.isSessionClipActive (trackId));
            expect (! mixer.isSessionClipQueued (trackId));
        }

        beginTest ("two different tracks loop their own session clips independently")
        {
            PluginManager pm;
            Mixer mixer (pm);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& trackA = project.addTrack (TrackType::audio, "A");
            auto& trackB = project.addTrack (TrackType::audio, "B");

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;

            mixer.launchSessionClip (trackA.getId(), clip, 0);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;
            mixer.process (out, 0, midi, noInput, true);

            expect (mixer.isSessionClipActive (trackA.getId()));
            expect (! mixer.isSessionClipActive (trackB.getId()));

            mixer.launchSessionClip (trackB.getId(), clip, 512);
            mixer.process (out, 512, midi, noInput, true);

            expect (mixer.isSessionClipActive (trackA.getId()));
            expect (mixer.isSessionClipActive (trackB.getId()));
        }
```

- [ ] **Step 2: Run tests to verify they fail**

Same build+run as Task 1. Expected: compile failure — `launchSessionClip`/`stopSessionClip`/`isSessionClipActive`/`isSessionClipQueued` do not exist on `Mixer` yet.

- [ ] **Step 3: Implement**

In `Source/Mixer/Mixer.h`, add to the public section of `class Mixer`, after `setPreviewClip`:

```cpp
        /** Launches a Session view clip on `track`, looping until stopped or
            replaced.  `launchAtSample` is an absolute sample on the SAME
            timeline SessionClock::nextBarBoundarySample() returns - Mixer
            does not know about bars or tempo, only "at this sample, swap." */
        void launchSessionClip (TrackId track, const SessionClip& clip, juce::int64 launchAtSample);
        /** Schedules the track's session clip to stop at `stopAtSample`. */
        void stopSessionClip (TrackId track, juce::int64 stopAtSample);
        /** True while a session clip is audibly looping on `track` right now. */
        bool isSessionClipActive (TrackId track) const noexcept;
        /** True while a launch/stop is scheduled but its boundary sample has
            not been reached yet. */
        bool isSessionClipQueued (TrackId track) const noexcept;
```

In `Source/Mixer/Mixer.cpp`, add to `struct Impl` (inside `TrackState`, alongside its existing fields):

```cpp
        // --- session clip hand-off (lock-free, one ring buffer per track) ----
        struct SessionRequest
        {
            SessionClip::Kind kind = SessionClip::Kind::midi;
            bool stop = false;
            juce::int64 launchAtSample = 0;
            juce::int64 lengthSamples = 0;
            std::vector<TimedMidi> events;          // kind == midi, built on the message thread
            juce::AudioSampleBuffer audioBuffer;    // kind == audio, loaded on the message thread
        };
        static constexpr int numSessionRequestSlots = 4;   // matches numPreviewSlots' ring depth
        SessionRequest sessionRequests[numSessionRequestSlots];
        std::atomic<int> sessionRequestSlot { -1 };
        std::atomic<int> sessionRequestGeneration { 0 };
        int nextSessionRequestSlot = 0;                     // message thread only

        // --- audio thread only ------------------------------------------------
        int seenSessionGeneration = 0;
        bool sessionActive = false;
        int  pendingSessionSlot = -1;         // ring index to adopt once the boundary hits, -1 == none pending
        bool pendingSessionIsStop = false;
        juce::int64 pendingSessionLaunchAtSample = -1;
        int  activeSessionSlot = -1;          // ring index currently playing
        int  sessionMidiCursor = 0;
        juce::int64 sessionPosSamples = 0;
```

Add two new private methods to `Impl`, alongside `gatherPreview`:

```cpp
    void gatherSession (TrackState& state, int numSamples, juce::MidiBuffer& dest)
    {
        const auto& req = state.sessionRequests[(size_t) state.activeSessionSlot];
        const auto blockEnd = state.sessionPosSamples + numSamples;

        while (state.sessionMidiCursor < (int) req.events.size()
                && req.events[(size_t) state.sessionMidiCursor].sample < blockEnd)
        {
            const auto& e = req.events[(size_t) state.sessionMidiCursor];
            dest.addEvent (e.message, (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1,
                                                           e.sample - state.sessionPosSamples));
            ++state.sessionMidiCursor;
        }
    }

    // ponytail: a loop point that falls inside a single audio block (clip
    // shorter than one block) is not stitched sample-accurately for MIDI -
    // sessionMidiCursor only resets between blocks.  Real session clips are
    // always at least a beat long, vastly longer than one ~10ms block, so
    // this is not reachable in practice; a sub-block-accurate rewrite would
    // be needed only if sub-block-length clips become a real use case.
    void renderSessionAudio (TrackState& state, int numSamples, juce::AudioBuffer<float>& dest)
    {
        const auto& req = state.sessionRequests[(size_t) state.activeSessionSlot];
        const auto& src = req.audioBuffer;

        if (src.getNumSamples() == 0)
            return;

        int destOffset = 0;
        auto readPos = (int) state.sessionPosSamples;

        while (destOffset < numSamples)
        {
            const auto toCopy = juce::jmin (numSamples - destOffset, src.getNumSamples() - readPos);
            if (toCopy <= 0)
                break;

            for (int ch = 0; ch < dest.getNumChannels(); ++ch)
                dest.addFrom (ch, destOffset, src, juce::jmin (ch, src.getNumChannels() - 1), readPos, toCopy);

            destOffset += toCopy;
            readPos    += toCopy;

            if (readPos >= src.getNumSamples())
                readPos = 0;
        }
    }
```

In `Mixer::process()`'s per-track loop (`Mixer.cpp` around line 1365-1426 — the `for (size_t t = 0; ...)` loop), add the session hand-off + playback block right after the existing `if (previewing) im.gatherPreview (n, im.trackMidi);` line and before `im.applyAutomation (state, strip, blockBeat);`:

```cpp
        // --- session clip hand-off (lock-free, per track) --------------------
        const auto sessionGen = state.sessionRequestGeneration.load (std::memory_order_acquire);

        if (sessionGen != state.seenSessionGeneration)
        {
            state.seenSessionGeneration = sessionGen;
            state.pendingSessionSlot          = state.sessionRequestSlot.load (std::memory_order_acquire);
            state.pendingSessionIsStop        = state.sessionRequests[(size_t) state.pendingSessionSlot].stop;
            state.pendingSessionLaunchAtSample = state.sessionRequests[(size_t) state.pendingSessionSlot].launchAtSample;
        }

        if (state.pendingSessionLaunchAtSample >= 0 && positionSamples + n >= state.pendingSessionLaunchAtSample)
        {
            if (state.pendingSessionIsStop)
            {
                state.sessionActive = false;
            }
            else
            {
                state.activeSessionSlot  = state.pendingSessionSlot;
                state.sessionPosSamples  = 0;
                state.sessionMidiCursor  = 0;
                state.sessionActive      = true;
            }

            state.pendingSessionLaunchAtSample = -1;
            state.pendingSessionSlot = -1;
        }

        if (state.sessionActive && state.activeSessionSlot >= 0)
        {
            const auto& activeReq = state.sessionRequests[(size_t) state.activeSessionSlot];

            if (activeReq.kind == SessionClip::Kind::midi)
                im.gatherSession (state, n, im.trackMidi);
            else
                im.renderSessionAudio (state, n, track);

            state.sessionPosSamples += n;

            if (activeReq.lengthSamples > 0 && state.sessionPosSamples >= activeReq.lengthSamples)
            {
                state.sessionPosSamples %= activeReq.lengthSamples;
                state.sessionMidiCursor = 0;
            }
        }
```

Add the public `Mixer` method implementations at the end of `Mixer.cpp`, alongside `setPreviewClip`:

```cpp
    void Mixer::launchSessionClip (TrackId trackId, const SessionClip& clip, juce::int64 launchAtSample)
    {
        auto& im = *impl;

        for (auto& statePtr : im.trackStates)
        {
            if (statePtr->id != trackId)
                continue;

            auto& state = *statePtr;
            const auto slot = state.nextSessionRequestSlot;
            state.nextSessionRequestSlot = (state.nextSessionRequestSlot + 1) % Impl::TrackState::numSessionRequestSlots;

            auto& req = state.sessionRequests[slot];
            req.kind = clip.kind;
            req.stop = false;
            req.launchAtSample = launchAtSample;

            if (clip.kind == SessionClip::Kind::midi)
            {
                req.events.clear();

                for (const auto& note : clip.notes)
                {
                    const auto offBeat = note.startBeats + note.lengthBeats;
                    req.events.push_back ({ im.beatsToSamples (note.startBeats),
                        juce::MidiMessage::noteOn (note.channel, note.pitch, (juce::uint8) note.velocity) });
                    req.events.push_back ({ im.beatsToSamples (offBeat),
                        juce::MidiMessage::noteOff (note.channel, note.pitch) });
                }

                std::stable_sort (req.events.begin(), req.events.end(),
                                  [] (const TimedMidi& a, const TimedMidi& b) { return a.sample < b.sample; });

                req.lengthSamples = im.beatsToSamples (clip.lengthBeats);
            }
            else
            {
                req.audioBuffer.setSize (0, 0);

                if (clip.sourceFile.existsAsFile())
                {
                    if (auto reader = std::unique_ptr<juce::AudioFormatReader> (im.formatManager.createReaderFor (clip.sourceFile)))
                    {
                        req.audioBuffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
                        reader->read (&req.audioBuffer, 0, (int) reader->lengthInSamples, 0, true, true);
                    }
                }

                req.lengthSamples = req.audioBuffer.getNumSamples();
            }

            state.sessionRequestSlot.store (slot, std::memory_order_release);
            state.sessionRequestGeneration.fetch_add (1, std::memory_order_release);
            return;
        }
    }

    void Mixer::stopSessionClip (TrackId trackId, juce::int64 stopAtSample)
    {
        auto& im = *impl;

        for (auto& statePtr : im.trackStates)
        {
            if (statePtr->id != trackId)
                continue;

            auto& state = *statePtr;
            const auto slot = state.nextSessionRequestSlot;
            state.nextSessionRequestSlot = (state.nextSessionRequestSlot + 1) % Impl::TrackState::numSessionRequestSlots;

            auto& req = state.sessionRequests[slot];
            req.stop = true;
            req.launchAtSample = stopAtSample;

            state.sessionRequestSlot.store (slot, std::memory_order_release);
            state.sessionRequestGeneration.fetch_add (1, std::memory_order_release);
            return;
        }
    }

    bool Mixer::isSessionClipActive (TrackId trackId) const noexcept
    {
        for (auto& statePtr : impl->trackStates)
            if (statePtr->id == trackId)
                return statePtr->sessionActive;

        return false;
    }

    bool Mixer::isSessionClipQueued (TrackId trackId) const noexcept
    {
        for (auto& statePtr : impl->trackStates)
            if (statePtr->id == trackId)
                return statePtr->pendingSessionLaunchAtSample >= 0;

        return false;
    }
```

`im.formatManager` here is `Impl::formatManager` (`Mixer.cpp:541`, already exists for clip playback) — confirm the exact member name by reading that line yourself; use whatever the real member is called if it differs. `numSessionRequestSlots` is declared `static constexpr` inside the nested `SessionRequest`-adjacent block of `TrackState` above; if C++20 in this project's configuration disallows referencing it as `Impl::TrackState::numSessionRequestSlots` from outside the class body the way it's written here, hoist the constant one scope up (to plain `Impl::numSessionRequestSlots`, alongside the existing `static constexpr int numPreviewSlots = 4;`) and reference it as `Impl::numSessionRequestSlots` instead — either placement is fine, match whichever compiles cleanly against this project's real `Impl` layout.

In `Source/Engine/AudioEngine.h`, add to the public section, after `previewMidiClip`/`stopPreview`:

```cpp
        // --- Session view (spec: Session view, Phase 3) -----------------------
        void launchSessionClip (TrackId, const SessionClip&);
        void stopSessionClip (TrackId);
        bool isSessionClipActive (TrackId) const noexcept;
        bool isSessionClipQueued (TrackId) const noexcept;
        SessionClock& getSessionClock() noexcept { return sessionClock; }
```

Add `#include "Engine/SessionClock.h"` to the top of `AudioEngine.h`, and add a private member alongside `Transport transport;`:

```cpp
        SessionClock sessionClock;
```

In `Source/Engine/AudioEngine.cpp`:
- Inside `audioDeviceIOCallbackWithContext` (wherever `transport.advance (numSamples)` — or the equivalent block-length variable — already runs), add `sessionClock.advance (numSamples);` right alongside it, unconditionally (not inside whatever `if` guards the transport-only logic, since the clock must advance every block regardless of play state). Read the real function body first to find the exact block-size variable name in scope at that point (it will not necessarily be called `numSamples`).
- Inside `AudioEngine::setProject(Project* p)`, add `sessionClock.reset();` — find the existing body first (it very likely already calls `transport.setProject(p)` and `mixer->setProject(p)`; add the reset call alongside those, not before them).
- Add the four wrapper methods near `previewMidiClip`/`stopPreview`:

```cpp
    void AudioEngine::launchSessionClip (TrackId trackId, const SessionClip& clip)
    {
        mixer->launchSessionClip (trackId, clip, sessionClock.nextBarBoundarySample (project->tempo));
    }

    void AudioEngine::stopSessionClip (TrackId trackId)
    {
        mixer->stopSessionClip (trackId, sessionClock.nextBarBoundarySample (project->tempo));
    }

    bool AudioEngine::isSessionClipActive (TrackId trackId) const noexcept
    {
        return mixer->isSessionClipActive (trackId);
    }

    bool AudioEngine::isSessionClipQueued (TrackId trackId) const noexcept
    {
        return mixer->isSessionClipQueued (trackId);
    }
```

`project->tempo` assumes `project` is non-null; guard with an early return (do nothing) if `project == nullptr`, matching how every other `AudioEngine` method already guards on the same member — check `previewMidiClip`'s own null handling and mirror it exactly.

- [ ] **Step 4: Run tests to verify they pass**

Same build+run. Expected: all four new `beginTest` blocks pass, 0 failed overall. Then perform the same mutation-verification standard this project's final docking-system review used (see `.superpowers/sdd/2026-08-26-docking-layout-system/progress.md` for the precedent): temporarily comment out the `positionSamples + n >= state.pendingSessionLaunchAtSample` boundary check so a launch fires immediately regardless of `launchAtSample`, rerun the suite, and confirm the "plays only after its boundary sample" test now fails. Restore the guard and confirm the suite is green again before committing.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Mixer/Mixer.h Source/Mixer/Mixer.cpp Source/Engine/AudioEngine.h Source/Engine/AudioEngine.cpp Source/Mixer/MixerTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add per-track quantized Session clip playback to Mixer/AudioEngine"
```

---

### Task 5: `SessionView` skeleton — grid layout, track headers, scene rows, scene management

**Files:**
- Create: `Source/UI/SessionView.h`
- Create: `Source/UI/SessionView.cpp`
- Modify: `Source/UI/MainComponent.cpp`

**Interfaces:**
- Consumes: `ProjectView` (`Source/UI/UiSupport.h:149-167`), `performProjectEdit` (`Source/UI/UiSupport.cpp:81-87`), `Project::scenes`/`addScene`/`removeScene` (Task 1), `Track::sessionSlots`/`findSessionClip` (Task 1), `AppContext`/`UiState` (existing).
- Produces: `class SessionView : public ProjectView` with a `paint`/`resized`/`changeListenerCallback` triad matching `GenerateView`'s shape, and nested `class SessionCell : public juce::Component` / `class SceneRowHeader : public juce::Component` sub-components. Task 6 (cell interaction) and Task 7 (drag-and-drop) both extend this same class.

- [ ] **Step 1: Write the skeleton (no failing-test cycle for pure layout — see note)**

Layout code has no meaningful assertion to TDD against (this project's own `MixerTests.cpp` draws the same line: "DSP quality is a listening test" — the UI equivalent here is "does the grid look right," which is a manual/visual check, not a unit test). Task 6 is where real, assertable behavior (click handling, scene management) gets TDD'd against this skeleton. Build the skeleton now; verify it compiles and renders by running the app, not by writing a test that asserts nothing meaningful.

Create `Source/UI/SessionView.h`:

```cpp
#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    class SessionView;

    /** One (track, scene) cell in the grid: empty (drop target only) or
        filled (shows the clip's name, coloured by its owning track). */
    class SessionCell final : public juce::Component
    {
    public:
        SessionCell (SessionView& ownerView, TrackId trackId, SceneId sceneId);

        void paint (juce::Graphics&) override;

    private:
        SessionView& owner;
        TrackId trackId;
        SceneId sceneId;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionCell)
    };

    /** Left-edge launch/stop toggle + name for one scene row. */
    class SceneRowHeader final : public juce::Component
    {
    public:
        SceneRowHeader (SessionView& ownerView, SceneId sceneId);

        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        SessionView& owner;
        SceneId sceneId;
        juce::TextButton launchButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneRowHeader)
    };

    /** Read-only mirror of a track's name/colour along the grid's top edge -
        editing stays in MixerView/TimelineView, the one place each control is
        owned (spec: Session view, Phase 3). */
    class SessionTrackHeader final : public juce::Component
    {
    public:
        explicit SessionTrackHeader (TrackId trackId);

        void paint (juce::Graphics&) override;

    private:
        TrackId trackId;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionTrackHeader)
    };

    //==============================================================================
    /** Track x scene clip launcher (spec: Session view, Phase 3). */
    class SessionView final : public ProjectView
    {
    public:
        SessionView (AppContext&, UiState&);
        ~SessionView() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        // --- used by SessionCell / SceneRowHeader ------------------------------
        Track* trackById (TrackId) noexcept;
        void addScene();
        void renameScene (SceneId, const juce::String& newName);
        void deleteScene (SceneId);
        void toggleSceneRow (SceneId);

        static constexpr int headerHeight = 28;
        static constexpr int rowHeight    = 56;
        static constexpr int columnWidth  = 120;
        static constexpr int sceneColumnWidth = 110;

    private:
        void rebuildGrid();

        juce::TextButton addSceneButton;
        juce::OwnedArray<SessionTrackHeader> trackHeaders;
        juce::OwnedArray<SceneRowHeader>     sceneHeaders;
        juce::OwnedArray<SessionCell>        cells;
        juce::Viewport gridViewport;
        juce::Component gridHolder;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionView)
    };
}
```

Create `Source/UI/SessionView.cpp`:

```cpp
#include "UI/SessionView.h"
#include "UI/MainComponent.h"

namespace ss
{
    //==============================================================================
    SessionCell::SessionCell (SessionView& ownerView, TrackId t, SceneId s)
        : owner (ownerView), trackId (t), sceneId (s)
    {
    }

    void SessionCell::paint (juce::Graphics& g)
    {
        auto bounds = getLocalBounds().reduced (2);
        auto* track = owner.trackById (trackId);
        auto* clip  = track != nullptr ? track->findSessionClip (sceneId) : nullptr;

        if (clip == nullptr)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.3f));
            g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 1.0f);
            return;
        }

        const auto colour = track != nullptr ? track->colour : juce::Colours::grey;
        g.setColour (colour.withAlpha (0.6f));
        g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (clip->name, bounds.reduced (4), juce::Justification::centredLeft, 2);
    }

    //==============================================================================
    SceneRowHeader::SceneRowHeader (SessionView& ownerView, SceneId s)
        : owner (ownerView), sceneId (s)
    {
        addAndMakeVisible (launchButton);
        launchButton.onClick = [this] { owner.toggleSceneRow (sceneId); };
    }

    void SceneRowHeader::resized()
    {
        launchButton.setBounds (getLocalBounds().reduced (4));
    }

    void SceneRowHeader::mouseDown (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem ("Rename", [this]
            {
                auto* aw = new juce::AlertWindow (TRANS ("Rename Scene"), {}, juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor ("name", launchButton.getButtonText());
                aw->addButton (TRANS ("OK"), 1);
                aw->addButton (TRANS ("Cancel"), 0);
                aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int result)
                {
                    if (result == 1)
                        owner.renameScene (sceneId, aw->getTextEditorContents ("name"));
                    delete aw;
                }));
            });
            menu.addItem ("Delete", [this] { owner.deleteScene (sceneId); });
            menu.showMenuAsync (juce::PopupMenu::Options());
        }
    }

    //==============================================================================
    SessionTrackHeader::SessionTrackHeader (TrackId t) : trackId (t) {}

    void SessionTrackHeader::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colours::black.withAlpha (0.15f));
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (juce::String ((juce::int64) trackId), getLocalBounds().reduced (4),
                          juce::Justification::centred, 1);
    }

    //==============================================================================
    SessionView::SessionView (AppContext& c, UiState& s) : ProjectView (c, s)
    {
        addAndMakeVisible (addSceneButton);
        addSceneButton.setButtonText (TRANS ("+ Scene"));
        addSceneButton.onClick = [this] { addScene(); };

        addAndMakeVisible (gridViewport);
        gridViewport.setViewedComponent (&gridHolder, false);

        rebuildGrid();
    }

    SessionView::~SessionView() = default;

    Track* SessionView::trackById (TrackId trackId) noexcept
    {
        return ctx.project != nullptr ? ctx.project->findTrack (trackId) : nullptr;
    }

    void SessionView::addScene()
    {
        if (ctx.project == nullptr)
            return;

        performProjectEdit (project(), TRANS ("Add scene"), [this]
        {
            project().addScene (TRANS ("Scene") + " " + juce::String (project().scenes.size() + 1));
        });
    }

    void SessionView::renameScene (SceneId sceneId, const juce::String& newName)
    {
        if (ctx.project == nullptr || newName.isEmpty())
            return;

        performProjectEdit (project(), TRANS ("Rename scene"), [this, sceneId, newName]
        {
            if (auto* scene = project().findScene (sceneId))
                scene->name = newName;
        });
    }

    void SessionView::deleteScene (SceneId sceneId)
    {
        if (ctx.project == nullptr)
            return;

        performProjectEdit (project(), TRANS ("Delete scene"), [this, sceneId]
        {
            project().removeScene (sceneId);
        });
    }

    void SessionView::toggleSceneRow (SceneId)
    {
        // Wired up in Task 6, once AudioEngine::launchSessionClip/stopSessionClip
        // and the active/queued query methods are in place to back a real toggle.
    }

    void SessionView::rebuildGrid()
    {
        trackHeaders.clear();
        sceneHeaders.clear();
        cells.clear();

        if (ctx.project == nullptr)
            return;

        const auto& tracks = project().getTracks();

        for (const auto& t : tracks)
        {
            auto* header = trackHeaders.add (new SessionTrackHeader (t->getId()));
            gridHolder.addAndMakeVisible (header);
        }

        for (const auto& scene : project().scenes)
        {
            auto* header = sceneHeaders.add (new SceneRowHeader (*this, scene.id));
            gridHolder.addAndMakeVisible (header);
        }

        for (const auto& scene : project().scenes)
            for (const auto& t : tracks)
                gridHolder.addAndMakeVisible (cells.add (new SessionCell (*this, t->getId(), scene.id)));

        resized();
    }

    void SessionView::paint (juce::Graphics& g)
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
    }

    void SessionView::resized()
    {
        auto area = getLocalBounds();
        addSceneButton.setBounds (area.removeFromBottom (28).removeFromLeft (100).reduced (2));
        gridViewport.setBounds (area);

        const auto& tracks = ctx.project != nullptr ? project().getTracks() : std::vector<std::unique_ptr<Track>>{};
        const int numTracks = (int) tracks.size();
        const int numScenes = ctx.project != nullptr ? (int) project().scenes.size() : 0;

        gridHolder.setSize (sceneColumnWidth + numTracks * columnWidth,
                            headerHeight + numScenes * rowHeight);

        for (int i = 0; i < trackHeaders.size(); ++i)
            trackHeaders[i]->setBounds (sceneColumnWidth + i * columnWidth, 0, columnWidth, headerHeight);

        for (int row = 0; row < sceneHeaders.size(); ++row)
            sceneHeaders[row]->setBounds (0, headerHeight + row * rowHeight, sceneColumnWidth, rowHeight);

        for (int row = 0; row < numScenes; ++row)
            for (int col = 0; col < numTracks; ++col)
                cells[row * numTracks + col]->setBounds (sceneColumnWidth + col * columnWidth,
                                                         headerHeight + row * rowHeight,
                                                         columnWidth, rowHeight);
    }

    void SessionView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        rebuildGrid();
        repaint();
    }
}
```

`SceneRowHeader` needs a way to show the scene's current name on its button, and `SessionCell`'s `paint()` above already calls `owner.trackById(...)` — both are child components, not subclasses, so neither can reach `ProjectView::project()` directly (it is `protected`, `Source/UI/UiSupport.h:161`). Add a small public forwarding accessor to `SessionView`, mirroring how `GenerateView` exposes `getUiState()` for `CandidateCard`'s use (`Source/UI/GenerateView.h:61`):

```cpp
        Project* getProject() const noexcept { return ctx.project.get(); }
```

Add this to its constructor body (after `addAndMakeVisible (launchButton);`):

```cpp
        if (auto* p = owner.getProject())
            if (auto* scene = p->findScene (s))
                launchButton.setButtonText (scene->name);
```

In `Source/UI/MainComponent.cpp`, change the `session` construction (the block quoted in the spec at `MainComponent.cpp:204-209`) from:

```cpp
    session = std::make_unique<PlaceholderView> (
        TRANS ("Session view"),
        TRANS ("A track x scene grid for launching clips non-linearly, with scene-wide firing and "
               "quantised launch. Generated candidates will be able to sit side by side in the cells "
               "for comparison."),
        TRANS ("Planned for Phase 3"));
```

to:

```cpp
    session = std::make_unique<SessionView> (ctx, uiState);
```

Add `#include "UI/SessionView.h"` near the top of `MainComponent.cpp`, alongside the other view includes. Confirm the member's declared type in `MainComponent.h`/`MainComponent.cpp`'s `Impl` (wherever `std::unique_ptr<PlaceholderView> session;` or similar is declared) is widened from `PlaceholderView` to a type both can share, or changed outright to `std::unique_ptr<SessionView> session;` — check whether `MainComponent` stores each view as its own concrete pointer type or as a common `juce::Component*`/`ProjectView*` in `panelsById`; if it is the latter (likely, since `panelsById` is a `std::map<juce::String, juce::Component*>` used generically across all 8 views per the spec's research), the member's own declared type just needs to change from `std::unique_ptr<PlaceholderView>` to `std::unique_ptr<SessionView>` and nothing else in `buildUi()` needs touching. Read the real declaration before assuming which is true.

- [ ] **Step 2: Build and run manually to verify**

Build (Debug config is fine for this manual check). Run `ScoreSmith.exe`, open the Session tab, confirm: a grid with column headers per existing track, an "+ Scene" button that adds a row when clicked, right-click on a scene name shows Rename/Delete, and the app does not crash. This is the "listening test" equivalent for layout — no automated assertion backs it, consistent with how DSP quality is treated elsewhere in this codebase.

- [ ] **Step 3: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/SessionView.h Source/UI/SessionView.cpp Source/UI/MainComponent.cpp Source/UI/MainComponent.h
git -C "E:/MIDI&DAW" commit -m "Add SessionView grid skeleton, replacing the Session placeholder"
```

---

### Task 6: Cell click-to-launch/stop and scene-row batch firing

**Files:**
- Modify: `Source/UI/SessionView.h`
- Modify: `Source/UI/SessionView.cpp`
- Test: `Source/UI/SessionViewTests.cpp` (new)

**Interfaces:**
- Consumes: `AudioEngine::launchSessionClip/stopSessionClip/isSessionClipActive/isSessionClipQueued` (Task 4), `SessionCell`/`SceneRowHeader`/`SessionView` (Task 5).
- Produces: `SessionCell::mouseDown` (click-to-launch/stop), `SessionView::toggleSceneRow` (real implementation), `SessionView::launchCell(TrackId, SceneId)` (a new public method — toggles launch/stop for that one cell's track, the single place the actual engine-calling logic lives; both `SessionCell::mouseDown` and `toggleSceneRow` call into it, and tests drive it directly).

- [ ] **Step 1: Write the failing tests**

Following this codebase's established `DockTests.cpp` pattern for view logic (drive handlers/state-changing methods directly, no real mouse events, no real audio device), create `Source/UI/SessionViewTests.cpp`:

```cpp
#include "UI/SessionView.h"
#include "Core/AppContext.h"

namespace ss
{

class SessionViewUnitTests final : public juce::UnitTest
{
public:
    SessionViewUnitTests() : juce::UnitTest ("SessionView", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("addScene / renameScene / deleteScene mutate the project")
        {
            AppContext ctx;   // its constructor already builds a working engine + a blank Project, no device opened
            UiState ui;

            SessionView view (ctx, ui);

            const auto before = (int) ctx.project->scenes.size();

            view.addScene();
            expectEquals ((int) ctx.project->scenes.size(), before + 1);
            const auto sceneId = ctx.project->scenes.back().id;

            view.renameScene (sceneId, "Chorus");
            expectEquals (ctx.project->scenes.back().name, juce::String ("Chorus"));

            view.deleteScene (sceneId);
            expectEquals ((int) ctx.project->scenes.size(), before);
        }

        beginTest ("launchCell starts playback on the clip's track through the engine")
        {
            // Build the whole project - track, scene, clip - BEFORE handing it to
            // ctx.setProject(), which synchronously rebuilds the mixer's track
            // states once. Adding a track to an already-installed project instead
            // would only reach the mixer via Project's async change broadcast,
            // which is not guaranteed to have run by the next line in a unit test.
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::audio, "A");
            auto& scene  = project->addScene ("Verse");

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            track.setSessionClip (scene.id, clip);

            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            expect (! ctx.engine->isSessionClipActive (trackId));
            expect (! ctx.engine->isSessionClipQueued (trackId));

            view.launchCell (trackId, sceneId);

            expect (ctx.engine->isSessionClipQueued (trackId) || ctx.engine->isSessionClipActive (trackId));
        }

        beginTest ("launchCell on an empty cell is a no-op")
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::audio, "A");
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            view.launchCell (trackId, sceneId);

            expect (! ctx.engine->isSessionClipQueued (trackId));
            expect (! ctx.engine->isSessionClipActive (trackId));
        }

        beginTest ("toggleSceneRow launches every filled cell in that row")
        {
            auto project = std::make_unique<Project>();
            auto& trackA = project->addTrack (TrackType::audio, "A");
            auto& trackB = project->addTrack (TrackType::audio, "B");
            auto& scene  = project->addScene ("Verse");

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            trackA.setSessionClip (scene.id, clip);
            trackB.setSessionClip (scene.id, clip);

            const auto trackAId = trackA.getId();
            const auto trackBId = trackB.getId();
            const auto sceneId  = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            view.toggleSceneRow (sceneId);

            expect (ctx.engine->isSessionClipQueued (trackAId) || ctx.engine->isSessionClipActive (trackAId));
            expect (ctx.engine->isSessionClipQueued (trackBId) || ctx.engine->isSessionClipActive (trackBId));
        }
    }
};

static SessionViewUnitTests sessionViewUnitTests;

}
```

`AppContext`'s constructor (`Source/Core/AppContext.cpp:11-`) already builds a real `Settings`/`PluginManager`/`AudioEngine` and installs a blank two-track `Project` via `setProject` — confirmed nothing opens a real audio device at that point (`AudioEngine::initialiseAudioDevice()` is a separate, explicit call `AppContext`'s constructor never makes). `AppContext::setProject(std::unique_ptr<Project>)` (`AppContext.cpp:72-77`) is the only correct way to swap in a test's own `Project` — it synchronously calls `engine->setProject(nullptr)` then `engine->setProject(project.get())`, which reaches `Mixer::setProject` and its synchronous `rebuild()`. Never assign `ctx.project` directly in a test; it would leave the engine's mixer still wired to whatever project `AppContext`'s constructor installed.

- [ ] **Step 2: Run tests to verify they fail**

Same build+run. Expected: compile failure (`SessionView::launchCell` does not exist yet).

- [ ] **Step 3: Implement**

In `Source/UI/SessionView.h`, add to the public section:

```cpp
        void launchCell (TrackId, SceneId);
```

In `Source/UI/SessionView.cpp`:

```cpp
    void SessionView::launchCell (TrackId trackId, SceneId sceneId)
    {
        if (ctx.project == nullptr || ctx.engine == nullptr)
            return;

        auto* track = trackById (trackId);
        auto* clip  = track != nullptr ? track->findSessionClip (sceneId) : nullptr;

        if (clip == nullptr)
            return;

        if (ctx.engine->isSessionClipActive (trackId) || ctx.engine->isSessionClipQueued (trackId))
            ctx.engine->stopSessionClip (trackId);
        else
            ctx.engine->launchSessionClip (trackId, *clip);
    }
```

Replace the `toggleSceneRow` stub with:

```cpp
    void SessionView::toggleSceneRow (SceneId sceneId)
    {
        if (ctx.project == nullptr)
            return;

        for (const auto& t : project().getTracks())
            if (t->findSessionClip (sceneId) != nullptr)
                launchCell (t->getId(), sceneId);
    }
```

Add `mouseDown` to `SessionCell` (`Source/UI/SessionView.h`, public section) and its implementation:

```cpp
        void mouseDown (const juce::MouseEvent&) override;
```

```cpp
    void SessionCell::mouseDown (const juce::MouseEvent&)
    {
        owner.launchCell (trackId, sceneId);
    }
```

Add a `juce::Timer`-driven repaint so active/queued state becomes visible without user interaction. In `Source/UI/SessionView.h`, add `private juce::Timer` to `SessionView`'s base list and declare:

```cpp
        void timerCallback() override;
        AudioEngine* getEngine() const noexcept { return ctx.engine.get(); }
```

In `Source/UI/SessionView.cpp`, in `SessionView`'s constructor, add `startTimerHz (10);`, and add:

```cpp
    void SessionView::timerCallback()
    {
        repaint();
    }
```

Replace `SessionCell::paint` (from Task 5) with:

```cpp
    void SessionCell::paint (juce::Graphics& g)
    {
        auto bounds = getLocalBounds().reduced (2);
        auto* track = owner.trackById (trackId);
        auto* clip  = track != nullptr ? track->findSessionClip (sceneId) : nullptr;

        if (clip == nullptr)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.3f));
            g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 1.0f);
            return;
        }

        const auto colour = track != nullptr ? track->colour : juce::Colours::grey;
        g.setColour (colour.withAlpha (0.6f));
        g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

        // ponytail: active/queued reflects the WHOLE track, not this specific
        // cell - a track with clips in more than one scene shows every one of
        // its filled cells as active/queued together, since nothing yet
        // records which scene id a track's currently-loaded clip came from.
        // Precise per-cell attribution needs SessionView to remember "the
        // last scene id launched per track"; add that if this proves
        // confusing in practice.
        if (auto* engine = owner.getEngine())
        {
            if (engine->isSessionClipActive (trackId))
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 2.0f);
            }
            else if (engine->isSessionClipQueued (trackId))
            {
                g.setColour (juce::Colours::white.withAlpha (0.5f));
                g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 1.0f);
            }
        }

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (clip->name, bounds.reduced (4), juce::Justification::centredLeft, 2);
    }
```

`SessionView.h` needs `#include "Core/AppContext.h"` (for `AudioEngine`) if it is not already reachable through `UI/UiSupport.h`'s own includes — check before assuming either way.

- [ ] **Step 4: Run tests to verify they pass**

Same build+run. Expected: all `SessionViewUnitTests` assertions pass, 0 failed overall.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/SessionView.h Source/UI/SessionView.cpp Source/UI/SessionViewTests.cpp
git -C "E:/MIDI&DAW" commit -m "Wire SessionView cells and scene rows to AudioEngine launch/stop"
```

---

### Task 7: Drag-and-drop — export a MIDI clip from the timeline, accept it in Session

**Context:** `TimelineView::mouseDown`/`mouseDrag` (`Source/UI/TimelineView.cpp:1722-`, `:1846-`) currently only ever move/resize/select clips within the timeline itself — there is no existing call to `juce::DragAndDropContainer::startDragging` anywhere in this file (confirmed by reading it; only `GenerateView::CandidateCard::mouseDrag`, `Source/UI/GenerateView.cpp:168-180`, does that anywhere in this codebase). This task adds a narrowly-scoped second gesture — holding the command/ctrl modifier while starting a drag on a MIDI clip's body exports it instead of moving it — rather than changing the existing move/resize gesture's default behavior.

**Files:**
- Modify: `Source/UI/TimelineView.h`
- Modify: `Source/UI/TimelineView.cpp`
- Modify: `Source/UI/SessionView.h`
- Modify: `Source/UI/SessionView.cpp`
- Test: `Source/UI/SessionViewTests.cpp` (extend)

**Interfaces:**
- Consumes: `juce::DragAndDropContainer::findParentDragContainerFor`/`startDragging` (JUCE, already used by `GenerateView::CandidateCard`), `UiState::dragPayload` (`Source/UI/UiSupport.h:98`, existing `std::shared_ptr<MidiClip>` field already used for the `"ss.candidate"` tag).
- Produces: a new drag description tag `"ss.timelineClip"` (parallel to the existing `"ss.candidate"` tag) carrying the same `UiState::dragPayload` shape; `SessionView`'s `isInterestedInDragSource`/`itemDropped` overrides (making `SessionView` a `juce::DragAndDropTarget`, alongside its existing `ProjectView` base).

- [ ] **Step 1: Write the failing test**

Add to `Source/UI/SessionViewTests.cpp`:

```cpp
        beginTest ("dropping a dragged MIDI clip writes a new SessionClip into the target cell")
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::midi, "Lead");
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            ui.dragPayload = std::make_shared<MidiClip> ();
            ui.dragPayload->name        = "Dragged riff";
            ui.dragPayload->lengthBeats = 6.0;
            ui.dragPayload->notes.push_back ({ 60, 0.0, 1.0, 100, 1, 1.0f });

            SessionView view (ctx, ui);

            juce::DragAndDropTarget::SourceDetails details (juce::var ("ss.timelineClip"), nullptr, {});
            view.handleClipDrop (details, trackId, sceneId);

            auto* clip = ctx.project->findTrack (trackId)->findSessionClip (sceneId);
            expect (clip != nullptr);
            if (clip != nullptr)
            {
                expect (clip->kind == SessionClip::Kind::midi);
                expectEquals (clip->name, juce::String ("Dragged riff"));
                expectWithinAbsoluteError (clip->lengthBeats, 6.0, 1.0e-9);
                expectEquals ((int) clip->notes.size(), 1);
            }
        }

        beginTest ("handleClipDrop ignores a drag with no payload")
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::midi, "Lead");
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;   // no dragPayload set
            SessionView view (ctx, ui);

            juce::DragAndDropTarget::SourceDetails details (juce::var ("ss.timelineClip"), nullptr, {});
            view.handleClipDrop (details, trackId, sceneId);

            expect (ctx.project->findTrack (trackId)->findSessionClip (sceneId) == nullptr);
        }
```

Confirm `juce::DragAndDropTarget::SourceDetails`'s real constructor signature yourself before trusting the two calls above verbatim (this plan's own precedent, Task 10 of the docking-layout-system plan, made the same request for the same class) — it takes a `juce::var description`, a `Component* sourceComponent`, and a `juce::Point<int> localPosition`, in that order, per the JUCE version already vendored in this project (`build/_deps/juce-src/modules/juce_gui_basics/mouse/juce_DragAndDropContainer.h`); re-verify against that real file rather than trusting this description.

- [ ] **Step 2: Run tests to verify they fail**

Same build+run. Expected: compile failure — `SessionView::handleClipDrop` does not exist yet.

- [ ] **Step 3: Implement**

In `Source/UI/SessionView.h`, add `public juce::DragAndDropTarget` to `SessionView`'s base list, and add to its public section:

```cpp
        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

        /** Applies a dropped clip's payload to one cell.  Public and taking
            the drop's TrackId/SceneId explicitly (rather than only the
            SourceDetails) so tests can drive it without a real drag
            gesture, matching this project's established `DockTests.cpp`
            pattern of calling handlers directly. */
        void handleClipDrop (const SourceDetails&, TrackId, SceneId);
```

In `Source/UI/SessionView.cpp`:

```cpp
    bool SessionView::isInterestedInDragSource (const SourceDetails& details)
    {
        return details.description.toString() == "ss.timelineClip"
            || details.description.toString() == "ss.candidate";
    }

    void SessionView::itemDropped (const SourceDetails& details)
    {
        // The drop landed on the view itself (between cells, or outside the
        // grid) rather than on a specific SessionCell - SessionCell handles
        // its own itemDropped for the precise (track, scene) target; this
        // top-level override exists only so isInterestedInDragSource makes
        // the view a valid drop target at all, per JUCE's requirement that a
        // parent claim interest before a child's own drop target is reached.
        juce::ignoreUnused (details);
    }

    void SessionView::handleClipDrop (const SourceDetails&, TrackId trackId, SceneId sceneId)
    {
        if (ctx.project == nullptr || ui.dragPayload == nullptr)
            return;

        auto* track = trackById (trackId);
        if (track == nullptr)
            return;

        const auto& dragged = *ui.dragPayload;

        performProjectEdit (project(), TRANS ("Add clip to session"), [this, trackId, sceneId, dragged]
        {
            SessionClip clip;
            clip.kind        = SessionClip::Kind::midi;
            clip.name        = dragged.name;
            clip.lengthBeats = dragged.lengthBeats;
            clip.notes       = dragged.notes;

            if (auto* t = trackById (trackId))
                t->setSessionClip (sceneId, clip);
        });
    }
```

Add drop handling to `SessionCell` itself (JUCE dispatches to the deepest interested `DragAndDropTarget` under the cursor, so `SessionCell` needs its own override to know which cell was hit — `SessionView::itemDropped` above only exists to satisfy the parent-must-claim-interest requirement):

In `Source/UI/SessionView.h`, add `public juce::DragAndDropTarget` to `SessionCell`'s base list and:

```cpp
        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;
```

In `Source/UI/SessionView.cpp`:

```cpp
    bool SessionCell::isInterestedInDragSource (const SourceDetails& details)
    {
        return details.description.toString() == "ss.timelineClip"
            || details.description.toString() == "ss.candidate";
    }

    void SessionCell::itemDropped (const SourceDetails& details)
    {
        owner.handleClipDrop (details, trackId, sceneId);
    }
```

Now the timeline export side. In `Source/UI/TimelineView.h`, add a private member alongside `dragMode`:

```cpp
        bool exportingClipDrag = false;
```

In `Source/UI/TimelineView.cpp`, in `mouseDown` at the point `dragMode` is set for a clip hit (the block computing `dragMode = e.position.x < r.getX() + edgeGrab ? DragMode::resizeStart : ...`), add right before that block:

```cpp
        exportingClipDrag = e.mods.isCommandDown() && ! ref.isAudio;
```

At the top of `mouseDrag` (`Source/UI/TimelineView.cpp:1846`), add, before the existing `if (dragMode == DragMode::none) return;` guard (or immediately after it if that guard already exists first):

```cpp
        if (exportingClipDrag && dragMode == DragMode::move && e.getDistanceFromDragStart() >= 8)
        {
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            {
                if (! selected.empty())
                {
                    const auto& sel = selected.front();
                    if (auto* track = project().findTrack (sel.track))
                        if (auto* clip = track->findMidiClip (sel.clip))
                        {
                            ui.dragPayload = std::make_shared<MidiClip> (*clip);
                            container->startDragging ("ss.timelineClip", this);
                        }
                }
            }

            exportingClipDrag = false;
            dragMode = DragMode::none;
            return;
        }
```

Reset `exportingClipDrag = false;` at the top of `mouseUp` alongside wherever `dragMode` itself gets reset back to `DragMode::none` there, so a stale flag never leaks into the next unrelated gesture. Read `mouseUp`'s real body first to find that reset point.

- [ ] **Step 4: Run tests to verify they pass**

Same build+run. Expected: the two new `SessionViewUnitTests` assertions pass, 0 failed overall.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/TimelineView.h Source/UI/TimelineView.cpp Source/UI/SessionView.h Source/UI/SessionView.cpp Source/UI/SessionViewTests.cpp
git -C "E:/MIDI&DAW" commit -m "Support command+drag export of a MIDI clip from Timeline into Session cells"
```

---

### Task 8: `GenerateView::sendCandidatesToSession`

**Files:**
- Modify: `Source/UI/GenerateView.h`
- Modify: `Source/UI/GenerateView.cpp`
- Create: `Source/UI/GenerateViewTests.cpp` (confirmed not to already exist)

**Interfaces:**
- Consumes: `Generator::Candidate`/`Generator::Part`/`Generator::toString` (`Source/AI/Generator.h:26-59`, existing), `performProjectEdit`, `Project::addScene`/`addTrack`, `Track::setSessionClip` (Tasks 1/5).
- Produces: `ss::sendCandidatesToSession (Project&, const std::vector<Generator::Candidate>&)` — a free function in `Source/UI/GenerateView.cpp`/`.h`, deliberately **not** a `GenerateView` method: `GenerateView::candidates` is a private member with no existing test seam (confirmed: `Glob Source/UI/*Tests.cpp` has no `GenerateViewTests.cpp` today, and nothing else in this codebase exercises `GenerateView`'s candidate list), and the actual logic needs nothing from `GenerateView` itself beyond `project()` and `candidates` — a free function taking both explicitly is fully testable with a hand-built `std::vector<Generator::Candidate>` and no live view at all. `GenerateView::sendCandidatesToSession()` becomes a two-line member that forwards to it.

- [ ] **Step 1: Write the failing test**

Create `Source/UI/GenerateViewTests.cpp` (confirmed to not already exist):

```cpp
#include "UI/GenerateView.h"
#include "Core/Project.h"

namespace ss
{

class GenerateViewUnitTests final : public juce::UnitTest
{
public:
    GenerateViewUnitTests() : juce::UnitTest ("GenerateView", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("sendCandidatesToSession creates one scene per candidate, one track per part, shared across candidates")
        {
            Project project;

            Generator::Candidate candidateA;
            candidateA.name = "Candidate A";
            MidiClip bassA;
            bassA.name = "Bass A";
            bassA.lengthBeats = 8.0;
            bassA.notes.push_back ({ 36, 0.0, 1.0, 100, 1, 1.0f });
            candidateA.parts.push_back ({ Generator::Part::bass, bassA });

            Generator::Candidate candidateB;
            candidateB.name = "Candidate B";
            MidiClip bassB;
            bassB.name = "Bass B";
            bassB.lengthBeats = 8.0;
            bassB.notes.push_back ({ 38, 0.0, 1.0, 100, 1, 1.0f });
            candidateB.parts.push_back ({ Generator::Part::bass, bassB });

            sendCandidatesToSession (project, { candidateA, candidateB });

            expectEquals ((int) project.scenes.size(), 2);
            expectEquals (project.scenes[0].name, juce::String ("Candidate A"));
            expectEquals (project.scenes[1].name, juce::String ("Candidate B"));

            // Exactly one bass track, shared by both candidates - not one each.
            int bassTracks = 0;
            for (const auto& t : project.getTracks())
                if (t->name.startsWith ("bass"))
                    ++bassTracks;
            expectEquals (bassTracks, 1);

            auto& track = project.getTrack (0);
            auto* clipA = track.findSessionClip (project.scenes[0].id);
            auto* clipB = track.findSessionClip (project.scenes[1].id);

            expect (clipA != nullptr);
            expect (clipB != nullptr);

            if (clipA != nullptr) expectEquals (clipA->name, juce::String ("Bass A"));
            if (clipB != nullptr) expectEquals (clipB->name, juce::String ("Bass B"));
        }

        beginTest ("sendCandidatesToSession on an empty candidate list is a no-op")
        {
            Project project;
            sendCandidatesToSession (project, {});
            expectEquals ((int) project.scenes.size(), 0);
        }
    }
};

static GenerateViewUnitTests generateViewUnitTests;

}
```

`Generator::toString (Part::bass)` — confirm the exact lowercase string it returns (`Source/AI/Generator.cpp:352`) before trusting the `t->name.startsWith ("bass")` check above; adjust the literal to match whatever case that function actually produces.

- [ ] **Step 2: Run test to verify it fails**

Same build+run. Expected: compile failure — `sendCandidatesToSession` does not exist yet.

- [ ] **Step 3: Implement**

Add to `Source/UI/GenerateView.h`, outside the class (free function, declared alongside the class in the same header):

```cpp
    /** Turns one generation batch into Session scenes: one scene per
        candidate, one track per part shared across every candidate in the
        batch (not one new track per candidate - the whole point is that
        candidates land in the same track column so Session's per-track
        exclusivity becomes an instant A/B switch between them).  A free
        function, not a GenerateView method, so it is testable without a
        live view (spec: Session view, Phase 3). */
    void sendCandidatesToSession (Project&, const std::vector<Generator::Candidate>&);
```

Add to `GenerateView`'s public section, in `Source/UI/GenerateView.h`:

```cpp
        void sendCandidatesToSession();
```

Add to `Source/UI/GenerateView.cpp`, near `adoptCandidate`:

```cpp
    void sendCandidatesToSession (Project& project, const std::vector<Generator::Candidate>& candidates)
    {
        std::map<Generator::Part, Track*> partTracks;   // one track per part, shared across every candidate in this batch

        for (const auto& candidate : candidates)
        {
            auto& scene = project.addScene (candidate.name);

            for (const auto& part : candidate.parts)
            {
                Track* track = nullptr;
                auto it = partTracks.find (part.first);

                if (it != partTracks.end())
                    track = it->second;
                else
                    track = &project.addTrack (TrackType::midi, Generator::toString (part.first));

                partTracks[part.first] = track;

                SessionClip clip;
                clip.kind        = SessionClip::Kind::midi;
                clip.name        = part.second.name.isNotEmpty() ? part.second.name : Generator::toString (part.first);
                clip.lengthBeats = part.second.lengthBeats;
                clip.notes       = part.second.notes;
                track->setSessionClip (scene.id, clip);
            }
        }
    }

    void GenerateView::sendCandidatesToSession()
    {
        if (ctx.project == nullptr || candidates.empty())
            return;

        performProjectEdit (project(), TRANS ("Send candidates to Session"),
                            [this] { ss::sendCandidatesToSession (project(), candidates); });

        ui.goTo (MainComponent::View::session);
    }
```

`candidate.parts` is `std::vector<std::pair<Part, MidiClip>>` (`Source/AI/Generator.h:48-59`) — `part.first`/`part.second` above match that shape; if Step 3's real read of that header shows a different field access pattern (e.g. structured bindings already used elsewhere in this file), match whatever `adoptCandidate`'s own loop over `candidate.parts` already does (`Source/UI/GenerateView.cpp:563`) exactly, for consistency within the same file. The free function's own name shadows nothing here since it lives at namespace scope (`ss::sendCandidatesToSession`) while `GenerateView::sendCandidatesToSession()` is a member — the explicit `ss::` qualification in the member's one-line body is required to disambiguate the call from a recursive self-call.

Add a button next to the existing per-candidate controls — in `GenerateView`'s constructor (near where `generateButton`/`abButton`/`stopButton` are wired up) and its `private:` member list, add:

```cpp
        juce::TextButton sendToSessionButton;
```

wired in the constructor:

```cpp
    addAndMakeVisible (sendToSessionButton);
    sendToSessionButton.setButtonText (TRANS ("Send to Session"));
    sendToSessionButton.onClick = [this] { sendCandidatesToSession(); };
```

and positioned in `resized()` alongside the other buttons — read the real `resized()` body first and place it in whatever row `generateButton`/`abButton`/`stopButton` already occupy, following that existing layout's own spacing convention rather than inventing a new one.

- [ ] **Step 4: Run tests to verify they pass**

Same build+run. Expected: the new test passes, 0 failed overall. Then run the FULL suite one final time and confirm the grand total matches (previous total) + (every assertion added across Tasks 1-8), 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/GenerateView.h Source/UI/GenerateView.cpp Source/UI/GenerateViewTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add GenerateView::sendCandidatesToSession for candidate A/B comparison in Session"
```

---

## After Task 8

Follow-ups deliberately left out of this plan, matching the spec's own "Explicitly out of scope for v1" section — do not build these unless a later plan asks for them:

- `UtauClip` session slots (no non-linear playback path exists for UTAU yet).
- A configurable quantize grid (1 bar is hard-coded throughout).
- Scene reordering (rows are append-only).
- In-place editing of a session clip's notes/audio (drag a fresh clip onto the cell to replace it instead).
- Per-cell precise active/queued attribution when a track has clips in multiple scenes (Task 6 accepted "any filled cell in the currently active/queued track" as v1 behavior — see that task's `ponytail:` note).
- A crossfade at the swap boundary for audio-kind clips (v1 is a hard cut).
- Sub-block-accurate MIDI loop stitching for a clip shorter than one audio block (Task 4's `ponytail:` note — not reachable with realistic clip lengths).
