# UTAU連携 Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load a classic `.ust` file into ScoreSmith, assign an installed UTAU voicebank, render it through an external resampler, and play the result back through the existing mixer — no in-app parameter editing yet (that's Phase 2).

**Architecture:** A new `TrackType::utau` clip type (`UtauClip`/`UtauNote`) holds UST data losslessly. `.ust` and `oto.ini` parsers turn files into that data. A renderer resolves each note's voicebank sample via direct lyric→alias match, shells out to the user's own `resampler.exe` per note (exactly like `Source/AI/StemSeparator.cpp` already shells out to Demucs), and stitches the fragments together natively with linear crossfades — no `wavtool.exe` dependency. The rendered WAV is cached on the clip and played back through the mixer's existing audio-clip machinery.

**Tech Stack:** C++20 / JUCE 8.0.6 (existing ScoreSmith stack, no new dependencies). External: the user's own UTAU/OpenUtau install supplies `resampler.exe` and voicebanks — configured via Settings, never bundled.

**Spec:** `E:\MIDI&DAW\docs\superpowers\specs\2026-08-26-utau-integration-phase1-design.md`

## Global Constraints

- No `.ustx` (OpenUtau-native format) support — `.ust` only.
- No VCV/CVVC alias auto-selection — lyric text must match an oto.ini alias exactly. Log a warning and render silence for unmatched notes.
- No pitchbend curve applied during rendering — every note renders at its flat pitch. PBS/PBW/PBY/PBM are still parsed and preserved for round-trip, just not fed to the resampler yet.
- No `wavtool.exe` dependency — fragment concatenation is native C++.
- Rendering is an explicit, user-triggered operation, never automatic on edit.
- ScoreSmith must never bundle or redistribute `resampler.exe`, UTAU, or any voicebank — only ever reference paths the user configures in Settings (same posture as the existing Demucs-compatible stem separator).
- Every new source file follows the existing style: `namespace ss { ... }`, `/** */` doc comments only where the *why* isn't obvious from the name, `juce::` fully qualified.

---

### Task 1: UTAU data types

**Files:**
- Modify: `Source/Core/Types.h` (the `enum class TrackType` declaration and the four free-function declarations right below it)
- Modify: `Source/Core/Types.cpp:50` (`toString (TrackType)`), `Source/Core/Types.cpp:86-89` (`trackTypeFromString`)
- Create: `Source/Core/UtauTypes.h`
- Create: `Source/Core/UtauTypes.cpp`
- Test: `Source/Core/UtauTypesTests.cpp`

**Interfaces:**
- Produces: `enum class TrackType { audio, midi, utau }`; `struct UtauPitchBend`; `struct UtauNote`; `struct UtauClip` with `double endBeats() const noexcept` and `juce::int64 currentContentHash() const noexcept`.

- [ ] **Step 1: Write the failing test**

Create `Source/Core/UtauTypesTests.cpp`:

```cpp
#include "Core/UtauTypes.h"
#include "Core/Types.h"
#include <juce_events/juce_events.h>

namespace ss
{

class UtauTypesUnitTests final : public juce::UnitTest
{
public:
    UtauTypesUnitTests() : juce::UnitTest ("ScoreSmith UTAU types", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("TrackType::utau round-trips through its string form");
        {
            expectEquals (toString (TrackType::utau), juce::String ("utau"));
            expect (trackTypeFromString ("utau") == TrackType::utau);
            expect (trackTypeFromString ("audio") == TrackType::audio);
            expect (trackTypeFromString ("midi") == TrackType::midi);
            expect (trackTypeFromString ("nonsense") == TrackType::midi,
                    "an unrecognised string should fall back to midi, matching the existing convention");
        }

        beginTest ("UtauClip::endBeats adds startBeats and lengthBeats");
        {
            UtauClip clip;
            clip.startBeats = 4.0;
            clip.lengthBeats = 8.0;
            expectWithinAbsoluteError (clip.endBeats(), 12.0, 1.0e-9);
        }

        beginTest ("UtauClip::currentContentHash changes when a note changes, stable otherwise");
        {
            UtauClip clip;
            clip.voicebankId = "TestBank";
            UtauNote note;
            note.lyric = "a";
            note.pitch = 60;
            clip.notes.push_back (note);

            const auto hash1 = clip.currentContentHash();
            const auto hash2 = clip.currentContentHash();
            expectEquals (hash1, hash2, "the same content must hash the same way every time");

            clip.notes[0].lyric = "i";
            expect (clip.currentContentHash() != hash1, "changing a note's lyric must change the hash");
        }
    }
};

static UtauTypesUnitTests utauTypesUnitTests;

}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```powershell
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
cmake --build "E:/MIDIDAW/build" --config Debug --parallel
```
Expected: FAIL — compile error, `UtauTypes.h` does not exist yet.

- [ ] **Step 3: Write the implementation**

Create `Source/Core/UtauTypes.h`:

```cpp
#pragma once
#include "Core/Types.h"
#include <juce_core/juce_core.h>
#include <vector>

/*  UTAU integration Phase 1 (docs/superpowers/specs/2026-08-26-utau-integration-
    phase1-design.md). These types mirror a classic .ust file closely enough to
    round-trip losslessly - see UstFile.h for the parser that produces them. */

namespace ss
{
    /** UST's PBS/PBW/PBY/PBM, kept as raw parsed values. Phase 1 does not apply
        this curve when rendering (every note renders at its flat pitch) - it is
        parsed and preserved purely so re-exporting a .ust does not lose data.
        ponytail: applying this curve for real means encoding the resampler's
        proprietary RLE pitch-string format, which needs a verified reference
        implementation to get bit-exact rather than reconstructing from memory -
        deferred past Phase 1 rather than risking silently wrong pitch. */
    struct UtauPitchBend
    {
        double startMs        = 0.0;   // PBS x
        double startSemitones = 0.0;   // PBS y
        std::vector<double>       widthsMs;          // PBW
        std::vector<double>       heightsSemitones;  // PBY
        std::vector<juce::String> curveTypes;        // PBM ("" | "s" | "r" | "j")
    };

    /** One UST note. `startBeats`/`lengthBeats` are RELATIVE to the owning
        UtauClip's startBeats, exactly like Note/MidiClip already work. */
    struct UtauNote
    {
        double startBeats  = 0.0;
        double lengthBeats = 1.0;
        int    pitch       = 60;      // NoteNum - MIDI note number
        juce::String lyric = "a";
        bool   isRest      = false;   // Lyric == "R"
        int    velocity    = 100;     // consonant speed, 0-200
        int    intensity   = 100;     // volume, 0-200
        int    modulation  = 0;       // pitch wobble depth, 0-100
        double preUtteranceMs = -1.0; // -1 == use the oto.ini value
        double voiceOverlapMs = -1.0; // -1 == use the oto.ini value
        juce::String flags;           // passed to the resampler verbatim
        UtauPitchBend pitchBend;
        juce::String envelope;        // raw "p1,p2,..." string, not edited in Phase 1
        /** Any .ust key not modelled above (future extensions, other tools'
            custom tags), so writeUstFile() can write it straight back out. */
        juce::NamedValueSet extra;

        double endBeats() const noexcept { return startBeats + lengthBeats; }
    };

    /** One UTAU clip on a TrackType::utau track. */
    struct UtauClip
    {
        ClipId id = invalidClipId;
        juce::String name;
        double startBeats  = 0.0;
        double lengthBeats = 4.0;
        std::vector<UtauNote> notes;

        juce::String voicebankId;          // VoicebankLibrary's identifier
        juce::File   renderedFile;         // cached render; empty == not rendered
        juce::int64  notesHashAtRender = 0; // currentContentHash() at the time renderedFile was produced

        double endBeats() const noexcept { return startBeats + lengthBeats; }

        /** Hash of everything that affects the rendered sound (notes + which
            voicebank). Compare against notesHashAtRender to know whether
            renderedFile is stale. */
        juce::int64 currentContentHash() const noexcept;
    };
}
```

Create `Source/Core/UtauTypes.cpp`:

```cpp
#include "Core/UtauTypes.h"

namespace ss
{
    juce::int64 UtauClip::currentContentHash() const noexcept
    {
        juce::String s = voicebankId;
        s << ";";

        for (const auto& n : notes)
        {
            s << n.startBeats << "|" << n.lengthBeats << "|" << n.pitch << "|" << n.lyric << "|"
              << n.velocity << "|" << n.intensity << "|" << n.modulation << "|"
              << n.preUtteranceMs << "|" << n.voiceOverlapMs << "|" << n.flags << ";";
        }

        return s.hashCode64();
    }
}
```

Modify `Source/Core/Types.h` — change the enum declaration:

```cpp
    enum class TrackType { audio, midi, utau };
```

Modify `Source/Core/Types.cpp:50`:

```cpp
    juce::String toString (TrackType t)
    {
        switch (t)
        {
            case TrackType::audio: return "audio";
            case TrackType::utau:  return "utau";
            default:                return "midi";
        }
    }
```

Modify `Source/Core/Types.cpp:86-89`:

```cpp
    TrackType trackTypeFromString (const juce::String& s)
    {
        if (s.equalsIgnoreCase ("audio")) return TrackType::audio;
        if (s.equalsIgnoreCase ("utau"))  return TrackType::utau;
        return TrackType::midi;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run the same build + test command as Step 2, then:
```powershell
& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe" --run-tests=ScoreSmith
```
Expected: PASS, including the new "ScoreSmith UTAU types" tests. Also check `TOTAL:` in `test-results.txt` grew by 5 (three `expect`/`expectEquals` in the first beginTest count as 4, plus 2 more — exact count isn't the point, zero failures is).

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/Types.h Source/Core/Types.cpp Source/Core/UtauTypes.h Source/Core/UtauTypes.cpp Source/Core/UtauTypesTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add UTAU data types (UtauNote/UtauClip) and TrackType::utau"
```

(If `E:\MIDI&DAW` is still not a git repository, skip this step in every task below — confirm with `git -C "E:/MIDI&DAW" status` first.)

---

### Task 2: Wire UtauClip into Track/Project

**Files:**
- Modify: `Source/Core/Project.h:2` (include), `Source/Core/Project.h:143-154` (Track class body)
- Modify: `Source/Core/Project.cpp:182-190` (`Track::endBeats()`)
- Test: `Source/Core/UtauTypesTests.cpp` (extend from Task 1)

**Interfaces:**
- Consumes: `UtauClip` from Task 1.
- Produces: `Track::utauClips` (`std::vector<UtauClip>`), `Track::findUtauClip(ClipId) noexcept -> UtauClip*`.

- [ ] **Step 1: Write the failing test**

Add to `Source/Core/UtauTypesTests.cpp`, inside `runTest()`, after the existing `beginTest` blocks:

```cpp
        beginTest ("Track::endBeats considers utauClips too");
        {
            Track track (1, TrackType::utau, "Vocal");
            UtauClip clip;
            clip.startBeats = 10.0;
            clip.lengthBeats = 5.0;
            track.utauClips.push_back (clip);

            expectWithinAbsoluteError (track.endBeats(), 15.0, 1.0e-9);
        }

        beginTest ("Track::findUtauClip finds by id, returns nullptr otherwise");
        {
            Track track (1, TrackType::utau, "Vocal");
            UtauClip clip;
            clip.id = 42;
            track.utauClips.push_back (clip);

            expect (track.findUtauClip (42) != nullptr);
            expect (track.findUtauClip (99) == nullptr);
        }
```

Add `#include "Core/Project.h"` to the top of `Source/Core/UtauTypesTests.cpp` (it currently only includes `Core/UtauTypes.h` and `Core/Types.h`).

- [ ] **Step 2: Run test to verify it fails**

Same build command as Task 1. Expected: FAIL — `Track` has no `utauClips` member and no `findUtauClip`.

- [ ] **Step 3: Write the implementation**

Modify `Source/Core/Project.h:2`, add right after the existing include:

```cpp
#include "Core/UtauTypes.h"
```

Modify `Source/Core/Project.h`, in the `Track` class body — add after the existing `std::vector<BuiltinFxSlot> builtinFx;` line (currently line 146):

```cpp
        std::vector<UtauClip>      utauClips;
```

And add after the existing `MidiClip* findMidiClip (ClipId) noexcept;` declaration (currently line 153):

```cpp
        UtauClip*  findUtauClip  (ClipId) noexcept;
```

Modify `Source/Core/Project.cpp` — add a new method right after `Track::findMidiClip` (find it near the top of the file, alongside `findAudioClip`):

```cpp
    UtauClip* Track::findUtauClip (ClipId clipId) noexcept
    {
        for (auto& c : utauClips)
            if (c.id == clipId)
                return &c;

        return nullptr;
    }
```

Modify `Source/Core/Project.cpp:182-190` (`Track::endBeats()`), add a loop over `utauClips`:

```cpp
    double Track::endBeats() const noexcept
    {
        double end = 0.0;

        for (const auto& c : audioClips) end = juce::jmax (end, c.endBeats());
        for (const auto& c : midiClips)  end = juce::jmax (end, c.endBeats());
        for (const auto& c : utauClips)  end = juce::jmax (end, c.endBeats());

        return end;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/Project.h Source/Core/Project.cpp Source/Core/UtauTypesTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add Track::utauClips and findUtauClip"
```

---

### Task 3: Shift-JIS / UTF-8 text decoder

**Files:**
- Create: `Source/IO/ShiftJis.h`
- Create: `Source/IO/ShiftJis.cpp`
- Test: `Source/IO/IoTests.cpp` (new file, will also hold Task 4/5 tests)

**Interfaces:**
- Produces: `juce::String ss::decodeUstText (const void* data, size_t numBytes)`.

- [ ] **Step 1: Write the failing test**

Create `Source/IO/IoTests.cpp`:

```cpp
#include "IO/ShiftJis.h"
#include <juce_events/juce_events.h>

namespace ss
{

class IoUnitTests final : public juce::UnitTest
{
public:
    IoUnitTests() : juce::UnitTest ("ScoreSmith IO", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("decodeUstText passes through valid UTF-8 unchanged");
        {
            const juce::String original = juce::CharPointer_UTF8 ("\xe3\x81\x82\xe3\x81\x84"); // "あい"
            const auto utf8Bytes = original.toUTF8();
            const auto decoded = decodeUstText (utf8Bytes.getAddress(), (size_t) original.getNumBytesAsUTF8());
            expectEquals (decoded, original);
        }

       #if JUCE_WINDOWS
        beginTest ("decodeUstText falls back to Shift-JIS on this machine");
        {
            // "あい" (hiragana a, i) in Shift-JIS is the byte pair sequence 82 A0 82 A2.
            const juce::uint8 sjisBytes[] = { 0x82, 0xA0, 0x82, 0xA2 };
            const auto decoded = decodeUstText (sjisBytes, sizeof (sjisBytes));
            expectEquals (decoded, juce::String (juce::CharPointer_UTF8 ("\xe3\x81\x82\xe3\x81\x84")));
        }
       #endif
    }
};

static IoUnitTests ioUnitTests;

}
```

- [ ] **Step 2: Run test to verify it fails**

Same build command as Task 1. Expected: FAIL — `IO/ShiftJis.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/IO/ShiftJis.h`:

```cpp
#pragma once
#include <juce_core/juce_core.h>

namespace ss
{
    /** Classic .ust / oto.ini files are frequently Shift-JIS, not UTF-8. Tries
        UTF-8 first and falls back to the platform's own Shift-JIS conversion
        table (Win32 MultiByteToWideChar / macOS CFString) when the bytes are
        not valid UTF-8 - deliberately not a hand-rolled JIS X 0208 table,
        which would risk silent, hard-to-catch mis-decoding of lyrics. */
    juce::String decodeUstText (const void* data, size_t numBytes);
}
```

Create `Source/IO/ShiftJis.cpp`:

```cpp
#include "IO/ShiftJis.h"

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace ss
{
namespace
{
    bool isValidUtf8 (const juce::uint8* data, size_t size)
    {
        size_t i = 0;

        while (i < size)
        {
            const auto b0 = data[i];
            size_t extra;

            if      ((b0 & 0x80) == 0x00) extra = 0;
            else if ((b0 & 0xE0) == 0xC0) extra = 1;
            else if ((b0 & 0xF0) == 0xE0) extra = 2;
            else if ((b0 & 0xF8) == 0xF0) extra = 3;
            else return false;

            if (i + extra >= size)
                return false;

            for (size_t k = 1; k <= extra; ++k)
                if ((data[i + k] & 0xC0) != 0x80)
                    return false;

            i += extra + 1;
        }

        return true;
    }

   #if JUCE_WINDOWS
    juce::String decodeShiftJis (const juce::uint8* data, size_t size)
    {
        if (size == 0)
            return {};

        const auto wideLen = ::MultiByteToWideChar (932, 0, (LPCSTR) data, (int) size, nullptr, 0);

        if (wideLen <= 0)
            return {};

        std::vector<WCHAR> wide ((size_t) wideLen);
        ::MultiByteToWideChar (932, 0, (LPCSTR) data, (int) size, wide.data(), wideLen);

        return juce::String (juce::CharPointer_UTF16 ((const juce::CharPointer_UTF16::CharType*) wide.data()),
                             (size_t) wideLen);
    }
   #elif JUCE_MAC
    juce::String decodeShiftJis (const juce::uint8* data, size_t size)
    {
        // ponytail: cannot be tested here - no Mac dev/test rig exists for this
        // project (see docs/STATUS.md's precedent for the same limitation on
        // the plugin-editor HWND-reparenting code). Uses CFString's own table
        // rather than a hand-rolled one, same reasoning as the Windows path.
        auto* cfData = CFDataCreate (kCFAllocatorDefault, data, (CFIndex) size);
        auto* cfString = CFStringCreateFromExternalRepresentation (kCFAllocatorDefault, cfData,
                                                                   kCFStringEncodingDOSJapanese);
        juce::String result;

        if (cfString != nullptr)
        {
            result = juce::String::fromCFString (cfString);
            CFRelease (cfString);
        }

        CFRelease (cfData);
        return result;
    }
   #else
    juce::String decodeShiftJis (const juce::uint8*, size_t) { return {}; }
   #endif
}

juce::String decodeUstText (const void* data, size_t numBytes)
{
    const auto* bytes = static_cast<const juce::uint8*> (data);

    if (isValidUtf8 (bytes, numBytes))
        return juce::String::fromUTF8 ((const char*) bytes, (int) numBytes);

    return decodeShiftJis (bytes, numBytes);
}
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4, checking the new "ScoreSmith IO" category tests pass.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/IO/ShiftJis.h Source/IO/ShiftJis.cpp Source/IO/IoTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add Shift-JIS/UTF-8 fallback decoder for UST/oto.ini text"
```

---

### Task 4: `.ust` parser and writer

**Files:**
- Create: `Source/IO/UstFile.h`
- Create: `Source/IO/UstFile.cpp`
- Modify: `Source/IO/IoTests.cpp` (extend)

**Interfaces:**
- Consumes: `ss::decodeUstText` (Task 3), `UtauNote`/`UtauPitchBend` (Task 1).
- Produces: `constexpr int ss::ustTicksPerBeat`; `std::vector<UtauNote> ss::parseUstFile (const void* data, size_t numBytes)`; `juce::String ss::writeUstFile (const std::vector<UtauNote>& notes)`.

- [ ] **Step 1: Write the failing test**

Add to `Source/IO/IoTests.cpp` (add `#include "IO/UstFile.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("parseUstFile reads two notes with correct positions and fields");
        {
            const juce::String ust =
                "[#VERSION]\r\n"
                "UST Version1.2\r\n"
                "[#SETTING]\r\n"
                "Tempo=120.00\r\n"
                "[#0000]\r\n"
                "Length=480\r\n"
                "Lyric=a\r\n"
                "NoteNum=60\r\n"
                "Velocity=100\r\n"
                "Intensity=100\r\n"
                "Modulation=0\r\n"
                "PreUtterance=\r\n"
                "Flags=\r\n"
                "PBS=0;0\r\n"
                "PBW=0\r\n"
                "PBY=\r\n"
                "[#0001]\r\n"
                "Length=240\r\n"
                "Lyric=i\r\n"
                "NoteNum=62\r\n"
                "Velocity=80\r\n"
                "Intensity=90\r\n"
                "Modulation=10\r\n"
                "PreUtterance=5.5\r\n"
                "Flags=g-5\r\n"
                "CustomTag=hello\r\n"
                "[#TRACKEND]\r\n";

            const auto notes = parseUstFile (ust.toRawUTF8(), (size_t) ust.getNumBytesAsUTF8());

            expectEquals ((int) notes.size(), 2);

            expectWithinAbsoluteError (notes[0].startBeats, 0.0, 1.0e-9);
            expectWithinAbsoluteError (notes[0].lengthBeats, 1.0, 1.0e-9); // 480 ticks == 1 beat
            expectEquals (notes[0].lyric, juce::String ("a"));
            expectEquals (notes[0].pitch, 60);
            expect (notes[0].preUtteranceMs < 0.0, "blank PreUtterance should mean \"use oto.ini's value\"");

            expectWithinAbsoluteError (notes[1].startBeats, 1.0, 1.0e-9); // right after note 0
            expectWithinAbsoluteError (notes[1].lengthBeats, 0.5, 1.0e-9); // 240 ticks == 0.5 beat
            expectEquals (notes[1].lyric, juce::String ("i"));
            expectEquals (notes[1].pitch, 62);
            expectEquals (notes[1].velocity, 80);
            expectWithinAbsoluteError (notes[1].preUtteranceMs, 5.5, 1.0e-9);
            expectEquals (notes[1].flags, juce::String ("g-5"));
            expectEquals (notes[1].extra["CustomTag"].toString(), juce::String ("hello"),
                         "an unrecognised key must be preserved, not dropped");
        }

        beginTest ("parseUstFile treats Lyric \"R\" as a rest");
        {
            const juce::String ust =
                "[#0000]\r\nLength=480\r\nLyric=R\r\nNoteNum=60\r\n[#TRACKEND]\r\n";
            const auto notes = parseUstFile (ust.toRawUTF8(), (size_t) ust.getNumBytesAsUTF8());
            expectEquals ((int) notes.size(), 1);
            expect (notes[0].isRest);
        }

        beginTest ("writeUstFile then parseUstFile round-trips the fields that matter");
        {
            UtauNote note;
            note.lyric = "ka";
            note.pitch = 67;
            note.lengthBeats = 2.0;
            note.velocity = 77;
            note.intensity = 88;
            note.modulation = 5;
            note.flags = "B10";
            note.extra.set ("CustomTag", "world");

            const auto text = writeUstFile ({ note });
            const auto roundTripped = parseUstFile (text.toRawUTF8(), (size_t) text.getNumBytesAsUTF8());

            expectEquals ((int) roundTripped.size(), 1);
            expectEquals (roundTripped[0].lyric, juce::String ("ka"));
            expectEquals (roundTripped[0].pitch, 67);
            expectWithinAbsoluteError (roundTripped[0].lengthBeats, 2.0, 1.0e-9);
            expectEquals (roundTripped[0].velocity, 77);
            expectEquals (roundTripped[0].flags, juce::String ("B10"));
            expectEquals (roundTripped[0].extra["CustomTag"].toString(), juce::String ("world"));
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `IO/UstFile.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/IO/UstFile.h`:

```cpp
#pragma once
#include "Core/UtauTypes.h"
#include <juce_core/juce_core.h>
#include <vector>

namespace ss
{
    /** Ticks per quarter note in the classic UST format (same convention as a
        480-PPQ MIDI file). */
    inline constexpr int ustTicksPerBeat = 480;

    /** Parses a .ust file's raw bytes into a flat, file-order note list.
        A .ust stores notes as a strictly sequential list with no explicit
        start position - startBeats is computed here by accumulating each
        note's own length. Decodes text via decodeUstText() (ShiftJis.h)
        internally, so callers can pass raw file bytes as-is. */
    std::vector<UtauNote> parseUstFile (const void* data, size_t numBytes);

    /** Writes `notes` back out in one-section-per-note .ust text, preserving
        each note's `extra` keys verbatim. Always UTF-8 - Phase 1 does not
        write Shift-JIS. */
    juce::String writeUstFile (const std::vector<UtauNote>& notes);
}
```

Create `Source/IO/UstFile.cpp`:

```cpp
#include "IO/UstFile.h"
#include "IO/ShiftJis.h"

namespace ss
{
namespace
{
    std::vector<double> parseCsvDoubles (const juce::String& csv)
    {
        std::vector<double> result;

        for (const auto& token : juce::StringArray::fromTokens (csv, ",", ""))
            result.push_back (token.trim().getDoubleValue());

        return result;
    }

    juce::String joinCsvDoubles (const std::vector<double>& values)
    {
        juce::StringArray tokens;

        for (auto v : values)
            tokens.add (juce::String (v, 3));

        return tokens.joinIntoString (",");
    }

    bool isKnownKey (const juce::String& key)
    {
        static const char* const known[] = {
            "Length", "Lyric", "NoteNum", "Velocity", "Intensity", "Modulation",
            "PreUtterance", "VoiceOverlap", "Flags", "Envelope", "PBS", "PBW", "PBY", "PBM"
        };

        for (auto* k : known)
            if (key == k)
                return true;

        return false;
    }

    UtauNote parseNoteSection (const juce::StringArray& lines)
    {
        UtauNote note;
        double lengthTicks = (double) ustTicksPerBeat;

        for (const auto& line : lines)
        {
            const auto eq = line.indexOfChar ('=');
            if (eq < 0) continue;

            const auto key   = line.substring (0, eq).trim();
            const auto value = line.substring (eq + 1);

            if      (key == "Length")       lengthTicks = value.getDoubleValue();
            else if (key == "Lyric")        { note.lyric = value; note.isRest = value.trim().equalsIgnoreCase ("R"); }
            else if (key == "NoteNum")      note.pitch = value.getIntValue();
            else if (key == "Velocity")     note.velocity = value.getIntValue();
            else if (key == "Intensity")    note.intensity = value.getIntValue();
            else if (key == "Modulation")   note.modulation = value.getIntValue();
            else if (key == "PreUtterance") note.preUtteranceMs = value.trim().isEmpty() ? -1.0 : value.getDoubleValue();
            else if (key == "VoiceOverlap") note.voiceOverlapMs = value.trim().isEmpty() ? -1.0 : value.getDoubleValue();
            else if (key == "Flags")        note.flags = value;
            else if (key == "Envelope")     note.envelope = value;
            else if (key == "PBS")
            {
                const auto parts = juce::StringArray::fromTokens (value, ";", "");
                note.pitchBend.startMs        = parts.size() > 0 ? parts[0].getDoubleValue() : 0.0;
                note.pitchBend.startSemitones = parts.size() > 1 ? parts[1].getDoubleValue() : 0.0;
            }
            else if (key == "PBW") note.pitchBend.widthsMs = parseCsvDoubles (value);
            else if (key == "PBY") note.pitchBend.heightsSemitones = parseCsvDoubles (value);
            else if (key == "PBM")
            {
                note.pitchBend.curveTypes.clear();
                for (const auto& t : juce::StringArray::fromTokens (value, ",", ""))
                    note.pitchBend.curveTypes.push_back (t);
            }
            else
                note.extra.set (juce::Identifier (key.removeCharacters (" \t")), value);
        }

        note.lengthBeats = lengthTicks / (double) ustTicksPerBeat;
        return note;
    }
}

std::vector<UtauNote> parseUstFile (const void* data, size_t numBytes)
{
    const auto text = decodeUstText (data, numBytes);
    std::vector<UtauNote> notes;

    juce::StringArray currentSection;
    bool inNoteSection = false;
    double cursorBeats = 0.0;

    auto flushSection = [&]
    {
        if (! inNoteSection || currentSection.isEmpty())
            return;

        auto note = parseNoteSection (currentSection);
        note.startBeats = cursorBeats;
        cursorBeats += note.lengthBeats;
        notes.push_back (note);
    };

    for (const auto& rawLine : juce::StringArray::fromLines (text))
    {
        const auto line = rawLine.trim();

        if (line.startsWithChar ('[') && line.endsWithChar (']'))
        {
            flushSection();
            currentSection.clear();

            const auto tag = line.substring (1, line.length() - 1);
            inNoteSection = tag.startsWithChar ('#')
                           && tag != "#VERSION" && tag != "#SETTING" && tag != "#TRACKEND"
                           && tag != "#PREV" && tag != "#NEXT" && tag != "#DELETE";
            continue;
        }

        if (inNoteSection && line.isNotEmpty())
            currentSection.add (line);
    }

    flushSection();
    return notes;
}

juce::String writeUstFile (const std::vector<UtauNote>& notes)
{
    juce::String out;
    out << "[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\nTempo=120.00\r\nTracks=1\r\nMode2=True\r\n";

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        out << "[#" << juce::String (i).paddedLeft ('0', 4) << "]\r\n";
        out << "Length=" << juce::String (juce::roundToInt (n.lengthBeats * (double) ustTicksPerBeat)) << "\r\n";
        out << "Lyric=" << (n.isRest ? juce::String ("R") : n.lyric) << "\r\n";
        out << "NoteNum=" << n.pitch << "\r\n";
        out << "Velocity=" << n.velocity << "\r\n";
        out << "Intensity=" << n.intensity << "\r\n";
        out << "Modulation=" << n.modulation << "\r\n";
        out << "PreUtterance=" << (n.preUtteranceMs >= 0.0 ? juce::String (n.preUtteranceMs, 3) : juce::String()) << "\r\n";
        out << "VoiceOverlap=" << (n.voiceOverlapMs >= 0.0 ? juce::String (n.voiceOverlapMs, 3) : juce::String()) << "\r\n";
        out << "Flags=" << n.flags << "\r\n";
        out << "PBS=" << juce::String (n.pitchBend.startMs, 3) << ";" << juce::String (n.pitchBend.startSemitones, 3) << "\r\n";
        out << "PBW=" << joinCsvDoubles (n.pitchBend.widthsMs) << "\r\n";
        out << "PBY=" << joinCsvDoubles (n.pitchBend.heightsSemitones) << "\r\n";

        {
            juce::StringArray pbm;
            for (auto& t : n.pitchBend.curveTypes) pbm.add (t);
            out << "PBM=" << pbm.joinIntoString (",") << "\r\n";
        }

        if (n.envelope.isNotEmpty())
            out << "Envelope=" << n.envelope << "\r\n";

        for (auto& kv : n.extra)
            out << kv.name.toString() << "=" << kv.value.toString() << "\r\n";
    }

    out << "[#TRACKEND]\r\n";
    return out;
}
}
```

Note for whoever implements this: `isKnownKey` above is currently unused (the parser's `else` branch handles "unknown" implicitly) — either wire it into an assertion/log for debugging, or remove it. Don't leave an unused function; if you remove it, the plan's intent (unknown keys go to `extra`) is unaffected.

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4. If `NamedValueSet`'s range-based-for or `Identifier` construction doesn't compile exactly as written, check `juce_core/containers/juce_NamedValueSet.h` in `E:/MIDIDAW/build/_deps/juce-src` for the exact API and adjust — the intent (store/retrieve arbitrary string key/value pairs) is what matters, not the exact call shown here.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/IO/UstFile.h Source/IO/UstFile.cpp Source/IO/IoTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add .ust parser and writer"
```

---

### Task 5: `oto.ini` parser

**Files:**
- Create: `Source/IO/OtoIni.h`
- Create: `Source/IO/OtoIni.cpp`
- Modify: `Source/IO/IoTests.cpp` (extend)

**Interfaces:**
- Consumes: `ss::decodeUstText` (Task 3).
- Produces: `struct OtoEntry { juce::String alias; juce::File sampleFile; double offset, consonant, cutoff, preUtterance, overlap; }`; `std::map<juce::String, OtoEntry> ss::parseOtoIni (const juce::String& text, const juce::File& sampleFolder)`; `std::map<juce::String, OtoEntry> ss::loadVoicebank (const juce::File& voicebankFolder)`.

- [ ] **Step 1: Write the failing test**

Add to `Source/IO/IoTests.cpp` (add `#include "IO/OtoIni.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("parseOtoIni reads alias, timings and resolves the sample path");
        {
            const juce::String oto =
                "a.wav=a,100.0,50.0,-200.0,30.0,10.0\r\n"
                "i.wav=,120.5,60.0,-150.0,25.0,5.0\r\n"; // no explicit alias -> falls back to the filename

            const juce::File folder ("C:/fake/voicebank");
            const auto entries = parseOtoIni (oto, folder);

            expectEquals ((int) entries.size(), 2);

            const auto aIt = entries.find ("a");
            expect (aIt != entries.end());
            expectEquals (aIt->second.sampleFile, folder.getChildFile ("a.wav"));
            expectWithinAbsoluteError (aIt->second.offset, 100.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.consonant, 50.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.cutoff, -200.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.preUtterance, 30.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.overlap, 10.0, 1.0e-9);

            const auto iIt = entries.find ("i");
            expect (iIt != entries.end(), "a blank alias field should fall back to the filename without extension");
        }

        beginTest ("parseOtoIni skips malformed lines instead of throwing");
        {
            const auto entries = parseOtoIni ("not a valid line\r\na.wav=a,1,2,3,4,5\r\n", juce::File ("C:/fake"));
            expectEquals ((int) entries.size(), 1);
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `IO/OtoIni.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/IO/OtoIni.h`:

```cpp
#pragma once
#include <juce_core/juce_core.h>
#include <map>

namespace ss
{
    /** One entry from a voicebank's oto.ini. Every timing value is passed
        straight through to the resampler CLI unchanged in Task 9's
        buildResamplerArgs() - ScoreSmith does not interpret oto.ini's sign
        conventions (e.g. a negative cutoff meaning "from the end of file")
        itself; the resampler already knows how to. */
    struct OtoEntry
    {
        juce::String alias;
        juce::File   sampleFile;
        double offset = 0.0, consonant = 0.0, cutoff = 0.0, preUtterance = 0.0, overlap = 0.0;
    };

    /** Parses one oto.ini file's already-decoded text (see ShiftJis.h) into
        alias -> entry. `sampleFolder` is where the referenced .wav filenames
        are resolved relative to. Lines that don't parse (no '=', too few
        comma fields) are skipped rather than treated as an error - a
        voicebank with one malformed line elsewhere should not become
        entirely unusable. */
    std::map<juce::String, OtoEntry> parseOtoIni (const juce::String& text, const juce::File& sampleFolder);

    /** Scans `voicebankFolder` and every subfolder for files literally named
        "oto.ini", merging all their entries (large voicebanks split entries
        across subfolders by UTAU convention - a later file's alias wins on a
        collision). Reads each file's raw bytes and decodes via
        decodeUstText() before parsing. */
    std::map<juce::String, OtoEntry> loadVoicebank (const juce::File& voicebankFolder);
}
```

Create `Source/IO/OtoIni.cpp`:

```cpp
#include "IO/OtoIni.h"
#include "IO/ShiftJis.h"

namespace ss
{
    std::map<juce::String, OtoEntry> parseOtoIni (const juce::String& text, const juce::File& sampleFolder)
    {
        std::map<juce::String, OtoEntry> result;

        for (const auto& rawLine : juce::StringArray::fromLines (text))
        {
            const auto line = rawLine.trim();
            if (line.isEmpty())
                continue;

            const auto eq = line.indexOfChar ('=');
            if (eq < 0)
                continue;

            const auto fileName = line.substring (0, eq).trim();
            const auto fields = juce::StringArray::fromTokens (line.substring (eq + 1), ",", "");

            if (fields.size() < 6)
                continue;

            OtoEntry entry;
            entry.alias        = fields[0].isNotEmpty() ? fields[0] : fileName.upToLastOccurrenceOf (".", false, false);
            entry.sampleFile   = sampleFolder.getChildFile (fileName);
            entry.offset       = fields[1].getDoubleValue();
            entry.consonant    = fields[2].getDoubleValue();
            entry.cutoff       = fields[3].getDoubleValue();
            entry.preUtterance = fields[4].getDoubleValue();
            entry.overlap      = fields[5].getDoubleValue();

            result[entry.alias] = entry;
        }

        return result;
    }

    std::map<juce::String, OtoEntry> loadVoicebank (const juce::File& voicebankFolder)
    {
        std::map<juce::String, OtoEntry> merged;

        for (const auto& otoFile : voicebankFolder.findChildFiles (juce::File::findFiles, true, "oto.ini"))
        {
            juce::MemoryBlock raw;
            otoFile.loadFileAsData (raw);
            const auto text = decodeUstText (raw.getData(), raw.getSize());

            for (auto& entry : parseOtoIni (text, otoFile.getParentDirectory()))
                merged[entry.first] = entry.second;
        }

        return merged;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/IO/OtoIni.h Source/IO/OtoIni.cpp Source/IO/IoTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add oto.ini voicebank parser"
```

---

### Task 6: Settings additions and Preferences UI

**Files:**
- Modify: `Source/Core/Settings.h` (add declarations near the existing `// plugins` / `// paths` groups)
- Modify: `Source/Core/Settings.cpp` (add implementations near `getSampleLibraryFolders`/`getStemSeparatorExecutable`)
- Modify: `Source/UI/PreferencesDialog.cpp` (`FilesTab`, where `getStemSeparatorExecutable` is already wired up)
- Test: none (this codebase has no automated tests for Settings' trivial property getters/setters or for any Preferences dialog — `docs/STATUS.md`'s test table lists only Engine/Mixer/AI/AutomationEditor/BasicSynth. Follow that precedent; verification here is "it builds and the new fields appear in Preferences", checked manually in Step 4.)

**Interfaces:**
- Produces: `juce::StringArray Settings::getUtauVoicebankFolders() const` / `void setUtauVoicebankFolders (const juce::StringArray&)`; `juce::File Settings::getUtauResamplerExecutable() const` / `void setUtauResamplerExecutable (const juce::File&)`.

- [ ] **Step 1: Add the declarations**

Modify `Source/Core/Settings.h`, add after the existing `getStemSeparatorExecutable`/`setStemSeparatorExecutable` line (in the `// AI (spec 8.2 - local inference only, v0.6)` group, or start a new `// UTAU (Phase 1)` group right after it):

```cpp
        // UTAU (Phase 1 - docs/superpowers/specs/2026-08-26-utau-integration-phase1-design.md)
        juce::StringArray getUtauVoicebankFolders() const;   void setUtauVoicebankFolders (const juce::StringArray&);
        juce::File getUtauResamplerExecutable() const;       void setUtauResamplerExecutable (const juce::File&);
```

- [ ] **Step 2: Add the implementations**

Modify `Source/Core/Settings.cpp`, add near `getSampleLibraryFolders`/`setSampleLibraryFolders` and `getStemSeparatorExecutable`/`setStemSeparatorExecutable` (mirror their exact bodies, just with new keys):

```cpp
    juce::StringArray Settings::getUtauVoicebankFolders() const
    {
        return splitPaths (props.getUserSettings()->getValue ("utauVoicebankFolders"));
    }

    void Settings::setUtauVoicebankFolders (const juce::StringArray& v)
    {
        props.getUserSettings()->setValue ("utauVoicebankFolders", v.joinIntoString ("\n"));
    }

    juce::File Settings::getUtauResamplerExecutable() const
    {
        return juce::File (props.getUserSettings()->getValue ("utauResamplerExe"));
    }

    void Settings::setUtauResamplerExecutable (const juce::File& f)
    {
        props.getUserSettings()->setValue ("utauResamplerExe", f.getFullPathName());
    }
```

(`splitPaths` is the existing private helper `getSampleLibraryFolders` already uses — find it near the top of `Settings.cpp` and confirm the name/signature before using it; it takes the raw newline-joined string and returns a `juce::StringArray`.)

- [ ] **Step 3: Add Preferences UI**

Modify `Source/UI/PreferencesDialog.cpp`'s `FilesTab` class. Two existing rows in that class are the exact templates to copy:
- `stemLabel`/`stemEditor` (`Source/UI/PreferencesDialog.cpp:1038-1045`) — a plain path `juce::TextEditor`, no Browse button, applied `onFocusLost`. Copy this shape for the resampler executable.
- `libraryLabel`/`libraryEditor`/`addLibraryButton` (`Source/UI/PreferencesDialog.cpp:1005-1036`) — a multi-line folder-list editor with a "Add folder..." button that appends via a `juce::FileChooser`. Copy this shape for the voicebank folders.

Add, right after the existing `modelLabel`/`modelEditor` block (`Source/UI/PreferencesDialog.cpp:1047-1053`):

```cpp
                setUpLabel (*this, utauResamplerLabel, TRANS ("UTAU resampler executable"));
                utauResamplerEditor.setText (ctx.settings->getUtauResamplerExecutable().getFullPathName(),
                                            juce::dontSendNotification);
                utauResamplerEditor.onFocusLost = [this]
                {
                    ctx.settings->setUtauResamplerExecutable (juce::File (utauResamplerEditor.getText().trim()));
                };
                addAndMakeVisible (utauResamplerEditor);

                setUpLabel (*this, utauFoldersLabel, TRANS ("UTAU voicebank folders (one per line)"));
                utauFoldersEditor.setMultiLine (true, false);
                utauFoldersEditor.setReturnKeyStartsNewLine (true);
                utauFoldersEditor.setText (ctx.settings->getUtauVoicebankFolders().joinIntoString ("\n"),
                                          juce::dontSendNotification);
                utauFoldersEditor.onFocusLost = [this]
                {
                    juce::StringArray folders;
                    folders.addLines (utauFoldersEditor.getText());
                    folders.removeEmptyStrings();
                    ctx.settings->setUtauVoicebankFolders (folders);
                };
                addAndMakeVisible (utauFoldersEditor);

                addUtauFolderButton.setButtonText (TRANS ("Add folder..."));
                addUtauFolderButton.onClick = [this]
                {
                    chooser = std::make_unique<juce::FileChooser> (TRANS ("Add a UTAU voicebank folder"));
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [this] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;

                        auto folders = ctx.settings->getUtauVoicebankFolders();
                        folders.addIfNotAlreadyThere (folder.getFullPathName());
                        ctx.settings->setUtauVoicebankFolders (folders);
                        utauFoldersEditor.setText (folders.joinIntoString ("\n"), juce::dontSendNotification);
                    });
                };
                addAndMakeVisible (addUtauFolderButton);
```

Add the matching member declarations to the existing member list at `Source/UI/PreferencesDialog.cpp:1119-1122` (extend those exact lines rather than adding a separate block):

```cpp
            juce::Label utauResamplerLabel, utauFoldersLabel;
            juce::TextEditor utauResamplerEditor, utauFoldersEditor;
            juce::TextButton addUtauFolderButton;
```

Modify the tab's `resized()` method. `Source/UI/PreferencesDialog.cpp:1080-1088` currently reads:

```cpp
                auto modelRow = Row::next (area);
                modelLabel.setBounds (modelRow.removeFromLeft (labelWidth));
                modelEditor.setBounds (modelRow);

                area.removeFromTop (6);
                auto libraryHeader = Row::next (area, 22);
                libraryLabel.setBounds (libraryHeader.removeFromLeft (300));
                addLibraryButton.setBounds (libraryHeader.removeFromRight (110));
                libraryEditor.setBounds (area);
```

Note `libraryEditor.setBounds (area)` consumes ALL remaining space (it's unbounded-height, the last thing laid out) — insert the new UTAU rows BETWEEN `modelRow` and that block, giving the new multi-line folder editor an explicit fixed height so `libraryEditor` still gets to consume whatever's left afterward:

```cpp
                auto modelRow = Row::next (area);
                modelLabel.setBounds (modelRow.removeFromLeft (labelWidth));
                modelEditor.setBounds (modelRow);

                auto utauResamplerRow = Row::next (area);
                utauResamplerLabel.setBounds (utauResamplerRow.removeFromLeft (labelWidth));
                utauResamplerEditor.setBounds (utauResamplerRow);

                area.removeFromTop (6);
                auto utauFoldersHeader = Row::next (area, 22);
                utauFoldersLabel.setBounds (utauFoldersHeader.removeFromLeft (300));
                addUtauFolderButton.setBounds (utauFoldersHeader.removeFromRight (110));
                utauFoldersEditor.setBounds (Row::next (area, 80)); // fixed height - libraryEditor below still needs the rest

                area.removeFromTop (6);
                auto libraryHeader = Row::next (area, 22);
                libraryLabel.setBounds (libraryHeader.removeFromLeft (300));
                addLibraryButton.setBounds (libraryHeader.removeFromRight (110));
                libraryEditor.setBounds (area);
```

- [ ] **Step 4: Verify it builds and the fields appear**

```powershell
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
cmake --build "E:/MIDIDAW/build" --config Debug --parallel
& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe"
```
Expected: builds with zero errors; launch the app, open Preferences → Files, confirm the two new UTAU fields are visible and that typing a path and reopening Preferences shows the value persisted (Settings already flushes to disk on every set, per the existing pattern).

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/Settings.h Source/Core/Settings.cpp Source/UI/PreferencesDialog.cpp
git -C "E:/MIDI&DAW" commit -m "Add UTAU voicebank/resampler settings and Preferences UI"
```

---

### Task 7: VoicebankLibrary

**Files:**
- Create: `Source/Vocal/VoicebankLibrary.h`
- Create: `Source/Vocal/VoicebankLibrary.cpp`
- Create: `Source/Vocal/VocalTests.cpp` (new file, will also hold Task 8/9/10 tests)

**Interfaces:**
- Consumes: `ss::loadVoicebank`/`OtoEntry` (Task 5), `Settings::getUtauVoicebankFolders` (Task 6).
- Produces:
  ```cpp
  class VoicebankLibrary
  {
  public:
      explicit VoicebankLibrary (Settings&);
      void refresh();                                       // rescans configured folders
      juce::StringArray getVoicebankIds() const;             // folder names, sorted
      const OtoEntry* findAlias (const juce::String& voicebankId, const juce::String& lyric) const;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `Source/Vocal/VocalTests.cpp`:

```cpp
#include "Vocal/VoicebankLibrary.h"
#include "Core/Settings.h"
#include <juce_events/juce_events.h>

namespace ss
{

class VocalUnitTests final : public juce::UnitTest
{
public:
    VocalUnitTests() : juce::UnitTest ("ScoreSmith vocal", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("VoicebankLibrary indexes a fake voicebank folder and resolves an alias");
        {
            auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithVocalTest")
                                .getChildFile (juce::String (juce::Random::getSystemRandom().nextInt()));
            auto bankFolder = tempRoot.getChildFile ("TestBank");
            bankFolder.createDirectory();

            bankFolder.getChildFile ("oto.ini")
                .replaceWithText ("a.wav=a,10.0,20.0,-30.0,5.0,2.0\r\n");
            bankFolder.getChildFile ("a.wav").create();

            Settings settings;
            settings.setUtauVoicebankFolders ({ tempRoot.getFullPathName() });

            VoicebankLibrary library (settings);
            library.refresh();

            const auto ids = library.getVoicebankIds();
            expect (ids.contains ("TestBank"), "the voicebank folder's name should be its id");

            const auto* entry = library.findAlias ("TestBank", "a");
            expect (entry != nullptr);
            expectWithinAbsoluteError (entry->offset, 10.0, 1.0e-9);

            expect (library.findAlias ("TestBank", "nonexistent") == nullptr);
            expect (library.findAlias ("NoSuchBank", "a") == nullptr);

            tempRoot.deleteRecursively();
        }
    }
};

static VocalUnitTests vocalUnitTests;

}
```

- [ ] **Step 2: Run test to verify it fails**

Same build command as Task 1. Expected: FAIL — `Vocal/VoicebankLibrary.h` does not exist (and the `Source/Vocal` directory doesn't exist yet — CMake's `CONFIGURE_DEPENDS` glob picks it up automatically once the files are created, no `CMakeLists.txt` edit needed).

- [ ] **Step 3: Write the implementation**

Create `Source/Vocal/VoicebankLibrary.h`:

```cpp
#pragma once
#include "Core/Settings.h"
#include "IO/OtoIni.h"
#include <map>

namespace ss
{
    /** Indexes the voicebanks configured in Settings::getUtauVoicebankFolders().
        Each configured folder's immediate subfolders are treated as one
        voicebank apiece (folder name == voicebank id), matching how UTAU's own
        voicebank picker organises things. */
    class VoicebankLibrary
    {
    public:
        explicit VoicebankLibrary (Settings& s) : settings (s) {}

        /** Rescans every configured folder. Call after the user changes
            Settings::getUtauVoicebankFolders(), and once at startup. */
        void refresh();

        juce::StringArray getVoicebankIds() const;

        /** Direct alias lookup only (Phase 1 - no VCV/CVVC connection logic).
            Returns nullptr if the voicebank id or the alias isn't found. */
        const OtoEntry* findAlias (const juce::String& voicebankId, const juce::String& lyric) const;

    private:
        Settings& settings;
        std::map<juce::String, std::map<juce::String, OtoEntry>> voicebanks; // id -> (alias -> entry)
    };
}
```

Create `Source/Vocal/VoicebankLibrary.cpp`:

```cpp
#include "Vocal/VoicebankLibrary.h"

namespace ss
{
    void VoicebankLibrary::refresh()
    {
        voicebanks.clear();

        for (const auto& folderPath : settings.getUtauVoicebankFolders())
        {
            const juce::File root (folderPath);

            if (! root.isDirectory())
                continue;

            for (const auto& sub : root.findChildFiles (juce::File::findDirectories, false))
                voicebanks[sub.getFileName()] = loadVoicebank (sub);
        }
    }

    juce::StringArray VoicebankLibrary::getVoicebankIds() const
    {
        juce::StringArray ids;

        for (const auto& [id, entries] : voicebanks)
            ids.add (id);

        ids.sort (false);
        return ids;
    }

    const OtoEntry* VoicebankLibrary::findAlias (const juce::String& voicebankId, const juce::String& lyric) const
    {
        const auto bankIt = voicebanks.find (voicebankId);
        if (bankIt == voicebanks.end())
            return nullptr;

        const auto aliasIt = bankIt->second.find (lyric);
        if (aliasIt == bankIt->second.end())
            return nullptr;

        return &aliasIt->second;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Vocal/VoicebankLibrary.h Source/Vocal/VoicebankLibrary.cpp Source/Vocal/VocalTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add VoicebankLibrary"
```

---

### Task 8: Native fragment concatenation (crossfade "wavtool")

**Files:**
- Create: `Source/Vocal/AudioStitcher.h`
- Create: `Source/Vocal/AudioStitcher.cpp`
- Modify: `Source/Vocal/VocalTests.cpp` (extend)

**Interfaces:**
- Produces:
  ```cpp
  struct StitchFragment { juce::AudioBuffer<float> audio; int overlapSamples = 0; };
  juce::AudioBuffer<float> stitchWithCrossfades (const std::vector<StitchFragment>& fragments);
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/Vocal/VocalTests.cpp` (add `#include "Vocal/AudioStitcher.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("stitchWithCrossfades concatenates with no overlap");
        {
            juce::AudioBuffer<float> a (1, 4), b (1, 4);
            a.clear(); a.setSample (0, 0, 1.0f);
            b.clear(); b.setSample (0, 0, 2.0f);

            const auto out = stitchWithCrossfades ({ { a, 0 }, { b, 0 } });

            expectEquals (out.getNumSamples(), 8);
            expectWithinAbsoluteError (out.getSample (0, 0), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (out.getSample (0, 4), 2.0f, 1.0e-6f);
        }

        beginTest ("stitchWithCrossfades keeps unity amplitude across a crossfade of two unity signals");
        {
            const int n = 100, overlap = 20;
            juce::AudioBuffer<float> a (1, n), b (1, n);

            for (int i = 0; i < n; ++i) { a.setSample (0, i, 1.0f); b.setSample (0, i, 1.0f); }

            const auto out = stitchWithCrossfades ({ { a, 0 }, { b, overlap } });

            expectEquals (out.getNumSamples(), n + n - overlap);

            for (int i = 0; i < out.getNumSamples(); ++i)
                expectWithinAbsoluteError (out.getSample (0, i), 1.0f, 1.0e-4f,
                                          "a linear crossfade between two unity signals must stay at unity, sample " + juce::String (i));
        }

        beginTest ("stitchWithCrossfades handles a single fragment and an empty list");
        {
            juce::AudioBuffer<float> a (1, 4);
            a.clear();
            a.setSample (0, 2, 5.0f);

            const auto single = stitchWithCrossfades ({ { a, 0 } });
            expectEquals (single.getNumSamples(), 4);
            expectWithinAbsoluteError (single.getSample (0, 2), 5.0f, 1.0e-6f);

            const auto empty = stitchWithCrossfades ({});
            expectEquals (empty.getNumSamples(), 0);
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `Vocal/AudioStitcher.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/Vocal/AudioStitcher.h`:

```cpp
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace ss
{
    struct StitchFragment
    {
        juce::AudioBuffer<float> audio;
        /** How many samples of THIS fragment's start overlap with the previous
            fragment's tail. Ignored (treated as 0) for the first fragment. */
        int overlapSamples = 0;
    };

    /** Concatenates fragments end-to-end, linearly crossfading each
        `overlapSamples`-long overlap so consecutive UTAU notes blend instead
        of clicking at the join. This is ScoreSmith's replacement for
        wavtool.exe (see docs/superpowers/specs/2026-08-26-utau-integration-
        phase1-design.md) - mono only, matching a vocal chain. */
    juce::AudioBuffer<float> stitchWithCrossfades (const std::vector<StitchFragment>& fragments);
}
```

Create `Source/Vocal/AudioStitcher.cpp`:

```cpp
#include "Vocal/AudioStitcher.h"

namespace ss
{
    juce::AudioBuffer<float> stitchWithCrossfades (const std::vector<StitchFragment>& fragments)
    {
        if (fragments.empty())
            return {};

        int totalLength = 0;

        for (size_t i = 0; i < fragments.size(); ++i)
        {
            const auto n = fragments[i].audio.getNumSamples();
            const auto overlap = i == 0 ? 0 : juce::jmin (fragments[i].overlapSamples, n);
            totalLength += n - overlap;
        }

        juce::AudioBuffer<float> out (1, juce::jmax (0, totalLength));
        out.clear();

        int writePos = 0;

        for (size_t i = 0; i < fragments.size(); ++i)
        {
            const auto& frag = fragments[i].audio;
            const auto n = frag.getNumSamples();
            const auto overlap = i == 0 ? 0 : juce::jmin (fragments[i].overlapSamples, n);
            const auto joinStart = writePos - overlap;

            for (int s = 0; s < n; ++s)
            {
                const auto destIndex = joinStart + s;
                if (destIndex < 0 || destIndex >= out.getNumSamples())
                    continue;

                float gain = 1.0f;

                if (overlap > 0 && s < overlap)
                {
                    gain = (float) s / (float) overlap;
                    // Fade out what's already written there (the previous
                    // fragment's tail) before adding this fragment's faded-in
                    // sample, so a unity+unity overlap still sums to unity.
                    const auto existing = out.getSample (0, destIndex);
                    out.setSample (0, destIndex, existing * (1.0f - gain));
                }

                out.addSample (0, destIndex, frag.getSample (0, s) * gain);
            }

            writePos = joinStart + n;
        }

        return out;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Vocal/AudioStitcher.h Source/Vocal/AudioStitcher.cpp Source/Vocal/VocalTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add native crossfade concatenation (wavtool replacement)"
```

---

### Task 9: Resampler CLI argument builder

**Files:**
- Create: `Source/Vocal/ResamplerArgs.h`
- Create: `Source/Vocal/ResamplerArgs.cpp`
- Modify: `Source/Vocal/VocalTests.cpp` (extend)

**Interfaces:**
- Consumes: `UtauNote` (Task 1), `OtoEntry` (Task 5).
- Produces: `juce::String ss::midiNoteToUtauPitch (int midiNoteNumber)`; `juce::StringArray ss::buildResamplerArgs (const juce::File& resamplerExe, const juce::File& inputWav, const juce::File& outputWav, const UtauNote& note, const OtoEntry& oto, double lengthMs)`.

- [ ] **Step 1: Write the failing test**

Add to `Source/Vocal/VocalTests.cpp` (add `#include "Vocal/ResamplerArgs.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("midiNoteToUtauPitch names notes correctly");
        {
            expectEquals (midiNoteToUtauPitch (60), juce::String ("C4"));
            expectEquals (midiNoteToUtauPitch (69), juce::String ("A4"));
            expectEquals (midiNoteToUtauPitch (61), juce::String ("C#4"));
        }

        beginTest ("buildResamplerArgs produces the classic 13-argument resampler CLI shape");
        {
            UtauNote note;
            note.pitch = 60;
            note.velocity = 90;
            note.intensity = 80;
            note.modulation = 5;
            note.flags = "g-5";
            note.preUtteranceMs = -1.0; // use oto's value
            note.voiceOverlapMs = -1.0;

            OtoEntry oto;
            oto.offset = 100.0;
            oto.consonant = 40.0;
            oto.cutoff = -200.0;
            oto.preUtterance = 30.0;
            oto.overlap = 15.0;

            const juce::File resampler ("C:/fake/resampler.exe");
            const juce::File input ("C:/fake/a.wav");
            const juce::File output ("C:/fake/out.wav");

            const auto args = buildResamplerArgs (resampler, input, output, note, oto, 500.0);

            // argv[0] is the executable itself, per juce::ChildProcess::start(StringArray) convention.
            expectEquals (args[0], resampler.getFullPathName());
            expectEquals (args[1], input.getFullPathName());
            expectEquals (args[2], output.getFullPathName());
            expectEquals (args[3], juce::String ("C4"));           // pitch
            expectEquals (args[4], juce::String ("90"));           // velocity
            expectEquals (args[5], juce::String ("g-5"));          // flags
            expectWithinAbsoluteError (args[6].getDoubleValue(), 130.0, 1.0e-6); // oto.offset + oto.preUtterance (note left it at -1)
            expectWithinAbsoluteError (args[7].getDoubleValue(), 500.0, 1.0e-6); // lengthMs
            expectWithinAbsoluteError (args[8].getDoubleValue(), 40.0, 1.0e-6);  // consonant
            expectWithinAbsoluteError (args[9].getDoubleValue(), -200.0, 1.0e-6); // cutoff, passed through unchanged
            expectEquals (args[10], juce::String ("80"));          // intensity
            expectEquals (args[11], juce::String ("5"));           // modulation
            expect (args.size() >= 13, "the trailing pitchbend slot must still be present, even if empty");
            expectEquals (args[args.size() - 1], juce::String(), "Phase 1 renders flat pitch - the pitchbend argument is always empty");
        }

        beginTest ("buildResamplerArgs prefers the note's own preUtterance/overlap when set");
        {
            UtauNote note;
            note.preUtteranceMs = 55.0;
            note.voiceOverlapMs = 12.0;

            OtoEntry oto;
            oto.offset = 100.0;
            oto.preUtterance = 999.0; // must be ignored - the note overrides it

            const auto args = buildResamplerArgs ({}, {}, {}, note, oto, 0.0);
            expectWithinAbsoluteError (args[6].getDoubleValue(), 155.0, 1.0e-6); // 100 (offset) + 55 (note's own preUtterance)
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `Vocal/ResamplerArgs.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/Vocal/ResamplerArgs.h`:

```cpp
#pragma once
#include "Core/UtauTypes.h"
#include "IO/OtoIni.h"
#include <juce_core/juce_core.h>

namespace ss
{
    /** Converts a MIDI note number to the note-name string the classic UTAU
        resampler CLI expects (e.g. 60 -> "C4"). */
    juce::String midiNoteToUtauPitch (int midiNoteNumber);

    /** Builds the argv for one note's resampler invocation, following the
        classic UTAU resampler CLI protocol:
            resampler.exe <in> <out> <pitch> <velocity> <flags> <offsetMs>
                          <lengthMs> <consonantMs> <cutoffMs> <intensity>
                          <modulation> <tempo> <pitchbend>
        `lengthMs` is precomputed by the caller (UtauRenderer, Task 10) from
        the project's real tempo map - this function has no tempo dependency
        of its own, which keeps it a pure, easily-tested string builder.
        Phase 1 always passes an empty pitchbend argument (flat pitch - see
        UtauPitchBend's doc comment for why). */
    juce::StringArray buildResamplerArgs (const juce::File& resamplerExe, const juce::File& inputWav,
                                          const juce::File& outputWav, const UtauNote& note,
                                          const OtoEntry& oto, double lengthMs);
}
```

Create `Source/Vocal/ResamplerArgs.cpp`:

```cpp
#include "Vocal/ResamplerArgs.h"

namespace ss
{
    juce::String midiNoteToUtauPitch (int midiNoteNumber)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto octave = midiNoteNumber / 12 - 1;
        const auto name = names[juce::jlimit (0, 11, midiNoteNumber % 12)];
        return juce::String (name) + juce::String (octave);
    }

    juce::StringArray buildResamplerArgs (const juce::File& resamplerExe, const juce::File& inputWav,
                                          const juce::File& outputWav, const UtauNote& note,
                                          const OtoEntry& oto, double lengthMs)
    {
        const auto preUtterance = note.preUtteranceMs >= 0.0 ? note.preUtteranceMs : oto.preUtterance;

        juce::StringArray args;
        args.add (resamplerExe.getFullPathName());
        args.add (inputWav.getFullPathName());
        args.add (outputWav.getFullPathName());
        args.add (midiNoteToUtauPitch (note.pitch));
        args.add (juce::String (note.velocity));
        args.add (note.flags);
        args.add (juce::String (oto.offset + preUtterance, 3));
        args.add (juce::String (lengthMs, 3));
        args.add (juce::String (oto.consonant, 3));
        args.add (juce::String (oto.cutoff, 3));
        args.add (juce::String (note.intensity));
        args.add (juce::String (note.modulation));
        args.add ("100");   // tempo - unused by most resamplers without a pitchbend curve, kept for CLI shape compatibility
        args.add ({});      // pitchbend - Phase 1 scope cut, see UtauPitchBend's doc comment
        return args;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Vocal/ResamplerArgs.h Source/Vocal/ResamplerArgs.cpp Source/Vocal/VocalTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add resampler CLI argument builder"
```

---

### Task 10: UtauRenderer orchestration

**Files:**
- Create: `Source/Vocal/UtauRenderer.h`
- Create: `Source/Vocal/UtauRenderer.cpp`
- Modify: `Source/Vocal/VocalTests.cpp` (extend)

**Interfaces:**
- Consumes: `UtauClip`/`UtauNote` (Task 1), `VoicebankLibrary` (Task 7), `stitchWithCrossfades`/`StitchFragment` (Task 8), `buildResamplerArgs` (Task 9).
- Produces:
  ```cpp
  class ResamplerRunner
  {
  public:
      virtual ~ResamplerRunner() = default;
      virtual bool run (const juce::StringArray& args, const juce::File& expectedOutputWav) = 0;
  };
  class ExternalResamplerRunner final : public ResamplerRunner { /* juce::ChildProcess-backed */ };

  struct RenderResult { bool ok = false; juce::String errorOrWarnings; juce::File renderedFile; juce::int64 contentHash = 0; };

  class UtauRenderer
  {
  public:
      UtauRenderer (VoicebankLibrary&, ResamplerRunner&, const juce::File& resamplerExecutable);
      RenderResult render (const UtauClip& clip, double tempoBpm, const juce::File& outputFolder);
  };
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/Vocal/VocalTests.cpp` (add `#include "Vocal/UtauRenderer.h"` and `#include <juce_audio_formats/juce_audio_formats.h>` at the top), inside `runTest()`:

```cpp
        beginTest ("UtauRenderer renders a two-note clip end to end with a fake resampler");
        {
            auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithRenderTest")
                                .getChildFile (juce::String (juce::Random::getSystemRandom().nextInt()));
            auto bankFolder = tempRoot.getChildFile ("Voicebanks").getChildFile ("TestBank");
            bankFolder.createDirectory();
            bankFolder.getChildFile ("oto.ini").replaceWithText ("a.wav=a,0.0,0.0,0.0,0.0,0.0\r\ni.wav=i,0.0,0.0,0.0,0.0,0.0\r\n");
            bankFolder.getChildFile ("a.wav").create();
            bankFolder.getChildFile ("i.wav").create();

            auto outputFolder = tempRoot.getChildFile ("Media");
            outputFolder.createDirectory();

            Settings settings;
            settings.setUtauVoicebankFolders ({ tempRoot.getChildFile ("Voicebanks").getFullPathName() });
            VoicebankLibrary library (settings);
            library.refresh();

            // Fake runner: instead of really invoking a resampler, writes a
            // short constant-amplitude mono WAV, so the test never depends on
            // an external executable being present on the machine.
            struct FakeRunner final : public ResamplerRunner
            {
                bool run (const juce::StringArray&, const juce::File& expectedOutputWav) override
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::FileOutputStream> stream (expectedOutputWav.createOutputStream());
                    std::unique_ptr<juce::AudioFormatWriter> writer (
                        wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));
                    if (writer == nullptr) return false;
                    stream.release();

                    juce::AudioBuffer<float> buffer (1, 4410); // 100ms of constant tone
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        buffer.setSample (0, i, 0.5f);
                    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
                    return true;
                }
            } fakeRunner;

            UtauClip clip;
            clip.voicebankId = "TestBank";
            UtauNote n1; n1.lyric = "a"; n1.lengthBeats = 1.0;
            UtauNote n2; n2.lyric = "i"; n2.lengthBeats = 1.0;
            clip.notes = { n1, n2 };

            UtauRenderer renderer (library, fakeRunner, juce::File ("C:/fake/resampler.exe"));
            const auto result = renderer.render (clip, 120.0, outputFolder);

            expect (result.ok, result.errorOrWarnings);
            expect (result.renderedFile.existsAsFile());

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result.renderedFile));
            expect (reader != nullptr);
            expect (reader->lengthInSamples > 0, "the rendered file should contain audio");

            tempRoot.deleteRecursively();
        }

        beginTest ("UtauRenderer reports ok with a warning when a lyric has no matching alias")
        {
            Settings settings;
            VoicebankLibrary library (settings); // no folders configured - every lookup misses

            struct AlwaysFailRunner final : public ResamplerRunner
            {
                bool run (const juce::StringArray&, const juce::File&) override { return false; }
            } runner;

            UtauClip clip;
            clip.voicebankId = "NoSuchBank";
            UtauNote n; n.lyric = "xyz"; n.lengthBeats = 1.0;
            clip.notes = { n };

            auto tempFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("ScoreSmithRenderTest2");
            tempFolder.createDirectory();

            UtauRenderer renderer (library, runner, juce::File ("C:/fake/resampler.exe"));
            const auto result = renderer.render (clip, 120.0, tempFolder);

            // An unmatched lyric renders as silence for that note rather than
            // failing the whole clip - the design doc requires "1ノートの失敗で
            // 全体を止めない".
            expect (result.ok);
            expect (result.errorOrWarnings.isNotEmpty(), "a warning should be recorded for the unmatched lyric");

            tempFolder.deleteRecursively();
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `Vocal/UtauRenderer.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/Vocal/UtauRenderer.h`:

```cpp
#pragma once
#include "Core/UtauTypes.h"
#include "Vocal/VoicebankLibrary.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace ss
{
    /** Runs one resampler invocation. Abstracted so tests never need a real
        resampler.exe on the machine - see ExternalResamplerRunner for the
        production implementation and VocalTests.cpp for a fake one. */
    class ResamplerRunner
    {
    public:
        virtual ~ResamplerRunner() = default;

        /** Runs the resampler with `args` (args[0] is the executable itself -
            juce::ChildProcess::start(StringArray) convention). Returns true
            only if the process succeeded AND `expectedOutputWav` now exists. */
        virtual bool run (const juce::StringArray& args, const juce::File& expectedOutputWav) = 0;
    };

    /** Production ResamplerRunner - shells out via juce::ChildProcess, exactly
        like Source/AI/StemSeparator.cpp already does for Demucs. */
    class ExternalResamplerRunner final : public ResamplerRunner
    {
    public:
        bool run (const juce::StringArray& args, const juce::File& expectedOutputWav) override;
    };

    struct RenderResult
    {
        bool ok = false;
        /** Empty on full success. Non-empty but ok==true means "rendered, but
            with per-note warnings" (e.g. an unmatched lyric rendered silent). */
        juce::String errorOrWarnings;
        juce::File renderedFile;
        juce::int64 contentHash = 0;
    };

    /** Renders a UtauClip: resolves each note's oto.ini alias by direct lyric
        match, runs the resampler per note, stitches the fragments with
        AudioStitcher, and writes one WAV to `outputFolder`. Never throws;
        a single unresolvable or failed note becomes silence for that note
        rather than failing the whole render. */
    class UtauRenderer
    {
    public:
        UtauRenderer (VoicebankLibrary& library, ResamplerRunner& runner, const juce::File& resamplerExecutable)
            : voicebanks (library), resamplerRunner (runner), resamplerExe (resamplerExecutable) {}

        RenderResult render (const UtauClip& clip, double tempoBpm, const juce::File& outputFolder);

    private:
        VoicebankLibrary& voicebanks;
        ResamplerRunner& resamplerRunner;
        juce::File resamplerExe;
    };
}
```

Create `Source/Vocal/UtauRenderer.cpp`:

```cpp
#include "Vocal/UtauRenderer.h"
#include "Vocal/AudioStitcher.h"
#include "Vocal/ResamplerArgs.h"
#include <juce_core/juce_core.h>

namespace ss
{
    bool ExternalResamplerRunner::run (const juce::StringArray& args, const juce::File& expectedOutputWav)
    {
        juce::ChildProcess process;

        if (! process.start (args))
            return false;

        process.waitForProcessToFinish (30000);
        return expectedOutputWav.existsAsFile();
    }

    RenderResult UtauRenderer::render (const UtauClip& clip, double tempoBpm, const juce::File& outputFolder)
    {
        RenderResult result;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const auto msPerBeat = 60000.0 / juce::jmax (1.0, tempoBpm);
        const auto tempDir = outputFolder.getChildFile (".utau_render_tmp");
        tempDir.createDirectory();

        std::vector<StitchFragment> fragments;
        juce::StringArray warnings;

        for (size_t i = 0; i < clip.notes.size(); ++i)
        {
            const auto& note = clip.notes[i];
            const auto lengthMs = note.lengthBeats * msPerBeat;

            if (note.isRest)
            {
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * 44100.0));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            const auto* oto = voicebanks.findAlias (clip.voicebankId, note.lyric);

            if (oto == nullptr)
            {
                warnings.add ("No voicebank alias for lyric \"" + note.lyric + "\" (note " + juce::String ((int) i) + ") - rendered silent.");
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * 44100.0));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            const auto noteOutputWav = tempDir.getChildFile ("note_" + juce::String ((int) i) + ".wav");
            noteOutputWav.deleteFile();

            const auto args = buildResamplerArgs (resamplerExe, oto->sampleFile, noteOutputWav, note, *oto, lengthMs);

            if (! resamplerRunner.run (args, noteOutputWav))
            {
                warnings.add ("Resampler failed for note " + juce::String ((int) i) + " (\"" + note.lyric + "\") - rendered silent.");
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * 44100.0));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (noteOutputWav));

            if (reader == nullptr)
            {
                warnings.add ("Could not read the resampler's output for note " + juce::String ((int) i) + " - rendered silent.");
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * 44100.0));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            juce::AudioBuffer<float> fragment (1, (int) reader->lengthInSamples);
            reader->read (&fragment, 0, fragment.getNumSamples(), 0, true, false);

            const auto overlapMs = note.voiceOverlapMs >= 0.0 ? note.voiceOverlapMs : oto->overlap;
            const auto overlapSamples = juce::jmax (0, (int) (overlapMs * 0.001 * reader->sampleRate));

            fragments.push_back ({ std::move (fragment), overlapSamples });
        }

        tempDir.deleteRecursively();

        const auto stitched = stitchWithCrossfades (fragments);

        outputFolder.createDirectory();
        const auto finalFile = outputFolder.getChildFile ("utau_clip_" + juce::String ((juce::int64) clip.id) + ".wav");
        finalFile.deleteFile();

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (finalFile.createOutputStream());

        if (stream == nullptr)
        {
            result.ok = false;
            result.errorOrWarnings = "Could not create " + finalFile.getFullPathName();
            return result;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));

        if (writer == nullptr)
        {
            result.ok = false;
            result.errorOrWarnings = "Could not write WAV to " + finalFile.getFullPathName();
            return result;
        }

        stream.release(); // the writer owns it from here
        writer->writeFromAudioSampleBuffer (stitched, 0, stitched.getNumSamples());
        writer.reset(); // flush before the caller reads the file back

        result.ok = true;
        result.errorOrWarnings = warnings.joinIntoString ("\n");
        result.renderedFile = finalFile;
        result.contentHash = clip.currentContentHash();
        return result;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Vocal/UtauRenderer.h Source/Vocal/UtauRenderer.cpp Source/Vocal/VocalTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add UtauRenderer orchestration"
```

---

### Task 11: Persistence

**Files:**
- Modify: `Source/Core/ProjectPersistence.cpp` (save side near `Source/Core/ProjectPersistence.cpp:328-341`, load side near `Source/Core/ProjectPersistence.cpp:518-532`)
- Modify: `Source/Core/UtauTypesTests.cpp` (extend)

**Interfaces:**
- Consumes: `UtauClip`/`UtauNote`/`UtauPitchBend` (Task 1).
- No new public interface — this task makes existing save/load round-trip `Track::utauClips`.

- [ ] **Step 1: Write the failing test**

Add to `Source/Core/UtauTypesTests.cpp` (add `#include "Core/ProjectPersistence.h"` if that header exists separately, otherwise the functions live directly on `Project` in `Project.h`, already included), inside `runTest()`:

```cpp
        beginTest ("Project save/load round-trips a UtauClip");
        {
            Project project;
            auto& track = project.addTrack (TrackType::utau, "Vocal");

            UtauClip clip;
            clip.id = 5;
            clip.name = "Verse 1";
            clip.startBeats = 4.0;
            clip.lengthBeats = 8.0;
            clip.voicebankId = "TestBank";
            clip.renderedFile = juce::File ("C:/fake/rendered.wav");
            clip.notesHashAtRender = 12345;

            UtauNote note;
            note.lyric = "ka";
            note.pitch = 64;
            note.startBeats = 0.0;
            note.lengthBeats = 1.0;
            note.velocity = 90;
            note.flags = "B10";
            note.pitchBend.startMs = 1.0;
            note.pitchBend.startSemitones = 2.0;
            note.pitchBend.widthsMs = { 10.0, 20.0 };
            clip.notes.push_back (note);

            track.utauClips.push_back (clip);

            const auto saved = project.toVar();

            Project reloaded;
            reloaded.loadFromVar (saved);

            expectEquals (reloaded.getNumTracks(), 1);
            expect (reloaded.getTrack (0).getType() == TrackType::utau);
            expectEquals ((int) reloaded.getTrack (0).utauClips.size(), 1);

            const auto& rc = reloaded.getTrack (0).utauClips[0];
            expectEquals (rc.name, juce::String ("Verse 1"));
            expectWithinAbsoluteError (rc.startBeats, 4.0, 1.0e-9);
            expectWithinAbsoluteError (rc.lengthBeats, 8.0, 1.0e-9);
            expectEquals (rc.voicebankId, juce::String ("TestBank"));
            expectEquals (rc.notesHashAtRender, (juce::int64) 12345);
            expectEquals ((int) rc.notes.size(), 1);
            expectEquals (rc.notes[0].lyric, juce::String ("ka"));
            expectEquals (rc.notes[0].pitch, 64);
            expectEquals (rc.notes[0].velocity, 90);
            expectEquals (rc.notes[0].flags, juce::String ("B10"));
            expectWithinAbsoluteError (rc.notes[0].pitchBend.startSemitones, 2.0, 1.0e-9);
            expectEquals ((int) rc.notes[0].pitchBend.widthsMs.size(), 2);
        }
```

(`Project::toVar() const` and `bool loadFromVar (const juce::var&)` are confirmed exact — `Source/Core/Project.h:207-208`. Neither takes a file parameter; `Project` resolves relative paths against its own `file` member internally, which is why `utauClipsToVar`/`utauClipsFromVar` below take an explicit `projectFile` parameter instead of reading an ambient member — they're free functions, not `Project` methods.)

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — reloaded track has zero `utauClips` (the save/load code doesn't handle them yet).

- [ ] **Step 3: Write the implementation**

Modify `Source/Core/ProjectPersistence.cpp` — add two helper functions near the existing `notesToVar`/`notesFromVar` (around line 35-72):

```cpp
        juce::var pitchBendToVar (const UtauPitchBend& pb)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("startMs", pb.startMs);
            obj->setProperty ("startSemitones", pb.startSemitones);

            juce::Array<juce::var> widths, heights, curves;
            for (auto v : pb.widthsMs) widths.add (v);
            for (auto v : pb.heightsSemitones) heights.add (v);
            for (auto& c : pb.curveTypes) curves.add (c);

            obj->setProperty ("widthsMs", widths);
            obj->setProperty ("heightsSemitones", heights);
            obj->setProperty ("curveTypes", curves);
            return juce::var (obj);
        }

        UtauPitchBend pitchBendFromVar (const juce::var& v)
        {
            UtauPitchBend pb;
            pb.startMs        = (double) v.getProperty ("startMs", 0.0);
            pb.startSemitones = (double) v.getProperty ("startSemitones", 0.0);

            if (auto* widths = v.getProperty ("widthsMs", {}).getArray())
                for (const auto& w : *widths) pb.widthsMs.push_back ((double) w);

            if (auto* heights = v.getProperty ("heightsSemitones", {}).getArray())
                for (const auto& h : *heights) pb.heightsSemitones.push_back ((double) h);

            if (auto* curves = v.getProperty ("curveTypes", {}).getArray())
                for (const auto& c : *curves) pb.curveTypes.push_back (c.toString());

            return pb;
        }

        juce::var utauClipsToVar (const std::vector<UtauClip>& clips, const juce::File& projectFile)
        {
            juce::Array<juce::var> array;

            for (const auto& c : clips)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id", (juce::int64) c.id);
                obj->setProperty ("name", c.name);
                obj->setProperty ("start", c.startBeats);
                obj->setProperty ("length", c.lengthBeats);
                obj->setProperty ("voicebank", c.voicebankId);
                obj->setProperty ("renderedFile", c.renderedFile.getFullPathName().isNotEmpty() && projectFile.existsAsFile()
                                                      ? c.renderedFile.getRelativePathFrom (projectFile.getParentDirectory())
                                                      : c.renderedFile.getFullPathName());
                obj->setProperty ("notesHash", (juce::int64) c.notesHashAtRender);

                juce::Array<juce::var> notes;

                for (const auto& n : c.notes)
                {
                    auto* noteObj = new juce::DynamicObject();
                    noteObj->setProperty ("start", n.startBeats);
                    noteObj->setProperty ("length", n.lengthBeats);
                    noteObj->setProperty ("pitch", n.pitch);
                    noteObj->setProperty ("lyric", n.lyric);
                    noteObj->setProperty ("isRest", n.isRest);
                    noteObj->setProperty ("velocity", n.velocity);
                    noteObj->setProperty ("intensity", n.intensity);
                    noteObj->setProperty ("modulation", n.modulation);
                    noteObj->setProperty ("preUtterance", n.preUtteranceMs);
                    noteObj->setProperty ("overlap", n.voiceOverlapMs);
                    noteObj->setProperty ("flags", n.flags);
                    noteObj->setProperty ("envelope", n.envelope);
                    noteObj->setProperty ("pitchBend", pitchBendToVar (n.pitchBend));
                    noteObj->setProperty ("extra", toVar (n.extra));
                    notes.add (juce::var (noteObj));
                }

                obj->setProperty ("notes", notes);
                array.add (juce::var (obj));
            }

            return array;
        }

        std::vector<UtauClip> utauClipsFromVar (const juce::var& v, const juce::File& projectFile)
        {
            std::vector<UtauClip> clips;

            if (auto* array = v.getArray())
            {
                for (const auto& item : *array)
                {
                    UtauClip clip;
                    clip.id          = (ClipId) (juce::int64) item.getProperty ("id", 0);
                    clip.name        = item.getProperty ("name", {}).toString();
                    clip.startBeats  = (double) item.getProperty ("start", 0.0);
                    clip.lengthBeats = (double) item.getProperty ("length", 4.0);
                    clip.voicebankId = item.getProperty ("voicebank", {}).toString();
                    clip.notesHashAtRender = (juce::int64) item.getProperty ("notesHash", 0);

                    const auto renderedPath = item.getProperty ("renderedFile", {}).toString();
                    if (renderedPath.isNotEmpty())
                        clip.renderedFile = juce::File::isAbsolutePath (renderedPath)
                                                ? juce::File (renderedPath)
                                                : projectFile.getParentDirectory().getChildFile (renderedPath);

                    if (auto* notes = item.getProperty ("notes", {}).getArray())
                    {
                        for (const auto& n : *notes)
                        {
                            UtauNote note;
                            note.startBeats     = (double) n.getProperty ("start", 0.0);
                            note.lengthBeats    = (double) n.getProperty ("length", 1.0);
                            note.pitch          = (int) n.getProperty ("pitch", 60);
                            note.lyric           = n.getProperty ("lyric", {}).toString();
                            note.isRest          = (bool) n.getProperty ("isRest", false);
                            note.velocity        = (int) n.getProperty ("velocity", 100);
                            note.intensity       = (int) n.getProperty ("intensity", 100);
                            note.modulation      = (int) n.getProperty ("modulation", 0);
                            note.preUtteranceMs  = (double) n.getProperty ("preUtterance", -1.0);
                            note.voiceOverlapMs  = (double) n.getProperty ("overlap", -1.0);
                            note.flags           = n.getProperty ("flags", {}).toString();
                            note.envelope        = n.getProperty ("envelope", {}).toString();
                            note.pitchBend       = pitchBendFromVar (n.getProperty ("pitchBend", {}));
                            note.extra           = namedValuesFrom (n.getProperty ("extra", {}));
                            clip.notes.push_back (std::move (note));
                        }
                    }

                    clips.push_back (std::move (clip));
                }
            }

            return clips;
        }
```

Modify the save side, `Source/Core/ProjectPersistence.cpp:341` — right after `obj->setProperty ("midiClips", midi);`, add:

```cpp
                obj->setProperty ("utauClips", utauClipsToVar (t->utauClips, file));
```

Modify the load side, `Source/Core/ProjectPersistence.cpp:532` — right after the `midiClips` block's closing `}`, add:

```cpp
                track->utauClips = utauClipsFromVar (item.getProperty ("utauClips", {}), file);

                for (const auto& c : track->utauClips)
                    lastClipId = juce::jmax (lastClipId, c.id);
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/ProjectPersistence.cpp Source/Core/UtauTypesTests.cpp
git -C "E:/MIDI&DAW" commit -m "Persist UtauClip/UtauNote in .ssproj"
```

---

### Task 12: Playback integration

**Files:**
- Modify: `Source/Mixer/Mixer.cpp:598-607` (`clipSignatureFor`), `Source/Mixer/Mixer.cpp:652-694` (`buildClips`)
- Modify: `Source/Mixer/MixerTests.cpp` (extend)

**Interfaces:**
- Consumes: `Track::utauClips` (Task 2).
- No new public interface — `ChannelStrip::process` already plays whatever `buildClips` sets up; this task only changes what `buildClips` considers.

- [ ] **Step 1: Write the failing test**

Add to `Source/Mixer/MixerTests.cpp` (add `#include "Core/UtauTypes.h"` and `#include <juce_audio_formats/juce_audio_formats.h>` at the top if not already present), inside `runTest()`:

```cpp
        beginTest ("a rendered UtauClip plays back through the mixer like an audio clip");
        {
            auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithMixerUtauTest_" + juce::String (juce::Random::getSystemRandom().nextInt()) + ".wav");

            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (tempFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));
                stream.release();
                juce::AudioBuffer<float> tone (1, 44100); // 1 second at 0.8 amplitude
                for (int i = 0; i < tone.getNumSamples(); ++i) tone.setSample (0, i, 0.8f);
                writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
            }

            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 } });
            auto& track = project.addTrack (TrackType::utau, "Vocal");

            UtauClip clip;
            clip.id = 1;
            clip.startBeats = 0.0;
            clip.lengthBeats = 4.0;
            clip.renderedFile = tempFile;
            track.utauClips.push_back (clip);

            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.setProject (&project);
            mixer.prepare (44100.0, 512, 2);
            mixer.rebuild();

            juce::AudioBuffer<float> output (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            // Give the buffering/resampling chain a few blocks to spin up before asserting.
            for (int i = 0; i < 20; ++i)
            {
                output.clear();
                mixer.process (output, i * 512, midi, noInput, true);
            }

            expect (output.getMagnitude (0, 0, 512) > 0.1f,
                    "a rendered UtauClip's audio should reach the mixer output");

            mixer.setProject (nullptr);
            tempFile.deleteFile();
        }
```

(The `Settings settings; PluginManager pluginManager (settings); Mixer mixer (pluginManager);` construction sequence above is copied verbatim from an existing test at `Source/Mixer/MixerTests.cpp:291-293` — confirmed exact.)

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — the UtauClip's rendered audio never reaches the mixer, so `output.getMagnitude(...)` stays at (or near) 0.

- [ ] **Step 3: Write the implementation**

Modify `Source/Mixer/Mixer.cpp` — add a helper right before `clipSignatureFor` (around line 598):

```cpp
    /*  A rendered UtauClip plays back exactly like an AudioClip once it has a
        renderedFile - rather than teaching ClipPlayback about a second clip
        type, both signature-building and clip-opening below iterate this
        combined view instead of track.audioClips directly. */
    static std::vector<AudioClip> effectiveAudioClipsFor (const Track& track)
    {
        std::vector<AudioClip> clips = track.audioClips;

        for (const auto& u : track.utauClips)
        {
            if (! u.renderedFile.existsAsFile())
                continue;

            AudioClip synthetic;
            synthetic.id = u.id;
            synthetic.sourceFile = u.renderedFile;
            synthetic.startBeats = u.startBeats;
            synthetic.lengthBeats = u.lengthBeats;
            clips.push_back (synthetic);
        }

        return clips;
    }
```

Modify `clipSignatureFor` (currently line 598-607) to use it:

```cpp
    static juce::String clipSignatureFor (const Track& track)
    {
        juce::String s;

        for (const auto& c : effectiveAudioClipsFor (track))
            s << c.sourceFile.getFullPathName() << "|" << c.startBeats << "|" << c.lengthBeats
              << "|" << c.offsetSeconds << "|" << c.playbackRate << "|" << (c.reversed ? 1 : 0) << ";";

        return s;
    }
```

Modify `buildClips` (currently line 652-694): replace every use of `track.audioClips` with a local `effectiveAudioClipsFor (track)` result:

```cpp
    void buildClips (TrackState& state, const Track& track)
    {
        const auto signature = clipSignatureFor (track);
        const auto effectiveClips = effectiveAudioClipsFor (track);

        if (signature == state.clipSignature && state.clips.size() == effectiveClips.size())
        {
            for (size_t i = 0; i < state.clips.size(); ++i)
                applyClipGain (*state.clips[i], effectiveClips[i]);

            return;
        }

        state.clips.clear();
        state.clipSignature = signature;

        for (const auto& clip : effectiveClips)
        {
            // ... unchanged body from here on ...
        }
    }
```

(The `// ... unchanged body ...` marker above means literally that: everything from `if (! clip.sourceFile.existsAsFile())` onward in the existing `buildClips` loop body stays exactly as it is today — only the two lines that reference `track.audioClips` change, to `effectiveClips`.)

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 4. Also re-run the FULL suite (not just the "ScoreSmith" category filter, which is everything anyway — just confirm the total assertion count grew and nothing existing regressed):

```powershell
& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe" --run-tests
Get-Content "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/test-results.txt" -Tail 3
```

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Mixer/Mixer.cpp Source/Mixer/MixerTests.cpp
git -C "E:/MIDI&DAW" commit -m "Play rendered UtauClips back through the existing audio-clip path"
```

---

## After Task 12

Phase 1 is functionally complete: `.ust` in, voicebank configured, `UtauRenderer::render()` produces a WAV, and it plays back. What's still missing before a user can do this from the UI (out of scope for this plan, needs its own follow-up task/plan):

- A menu action to import a `.ust` file into a new `TrackType::utau` track (wiring `parseUstFile` + file picker + `Track::utauClips.push_back`)
- A "Render" button/command that calls `UtauRenderer::render()` on the selected clip and writes the result back onto it (`clip.renderedFile = result.renderedFile; clip.notesHashAtRender = result.contentHash;`), plus a `VoicebankLibrary` instance owned somewhere reachable (likely `AppContext`, alongside `PluginManager`)
- A way to pick which configured voicebank a `UtauClip` uses (a combo box populated from `VoicebankLibrary::getVoicebankIds()`)
- Phase 2's dedicated editing UI

These are UI wiring, not new subsystems — small enough to fold into a short follow-up plan once Phase 1's pieces above are reviewed and merged.
