# DAWproject Interoperability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a KANADE DAW `Project` export to and import from `.dawproject`,
the open Bitwig/PreSonus interchange format supported by Studio One 6.5+,
Bitwig, and Cubase 14+, with tracks, clips, tempo, automation, and VST3
plugin state intact.

**Architecture:** One new IO module (`Source/IO/DawProject.h/.cpp`,
`Source/IO/DawProjectTests.cpp`) alongside the existing MIDI/MusicXML
exporters in `Source/IO/`. Export walks the `Project` tree once, building a
`juce::XmlElement` tree plus a set of extra zip entries (audio files, plugin
state blobs); import does the reverse. Two small archive-plumbing classes
(`ArchiveWriter`/`ArchiveReader`) wrap `juce::ZipFile`/`juce::ZipFile::Builder`
so every later task adds entries incrementally instead of re-deriving zip
handling. `MainComponent.cpp` gets two new File menu commands wired the same
way the existing MIDI/MusicXML commands are.

**Tech Stack:** JUCE 8.0.6 (`juce_core`'s `ZipFile`/`ZipFile::Builder`,
`XmlElement`/`parseXML`) — no new dependency. C++20. `juce::UnitTest` via
`ScoreSmith.exe --run-tests`.

**Spec:** `docs/superpowers/specs/2026-08-28-dawproject-interop-design.md`

## Global Constraints

- No new third-party dependency — zip and XML both come from `juce_core`,
  already linked (spec Tech Stack).
- Plugin format is VST3 only — KANADE DAW hosts no other format, so every
  `<Device>` this code ever writes or expects to read is `<Vst3Plugin>`
  (spec Scope).
- Nothing may be silently dropped. Every unmappable/skipped element (utau
  tracks, reverb/delay builtin FX, foreign devices, missing plugins)
  produces one `warningsOut` entry; only a genuinely malformed file
  (unreadable zip, missing `project.xml`, unparseable XML) is an `errorOut`
  failure. The two must never be conflated (spec Error Handling).
- Import must never partially mutate the `Project&` passed in on failure
  (spec Import). `Project` is non-copyable and non-movable, so this is
  achieved by ordering, not by a scratch-object swap: `importDawProject`
  (Task 9) resolves the only failable step — reading and parsing
  `project.xml` out of the archive — completely before any
  `parseStructure`/`parseClips`/`parseDevicesAndAutomation`/`parseScenes`
  call touches `project`. Once that gate passes, every later step only
  adds data or skips-with-a-warning; nothing after the gate can fail in a
  way that would need to be undone.
- Automation values are written/read as `unit="normalized"` across the
  board — gain, pan, mute, builtin FX, and plugin parameters alike.
  `Track::AutomationLane::points` is *always* a 0..1 breakpoint no matter
  the target (confirmed against `Source/Mixer/Mixer.cpp:961-973`: gain
  resolves via `-60.0f + 66.0f * v`, pan via `v*2-1`, mute via `v>=0.5`,
  builtin FX/plugin via `setParameterNormalised`/`setValue(v)` — all
  consume the same normalized `v`). No denormalization step anywhere in
  export, no dependency on a loaded plugin instance (spec Data Model).
- A new file under `Source/` never needs a `CMakeLists.txt` edit
  (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")`).

---

### Task 1: Archive plumbing (`ArchiveWriter`/`ArchiveReader`) + vendored schema

**Files:**
- Create: `docs/superpowers/specs/dawproject-schema/Project.xsd`
- Create: `docs/superpowers/specs/dawproject-schema/MetaData.xsd`
- Create: `Source/IO/DawProject.h`
- Create: `Source/IO/DawProjectTests.cpp`

**Interfaces:**
- Consumes: nothing from other tasks (first task). `DawProject.cpp` isn't
  created until Task 2 — `ArchiveWriter`/`ArchiveReader` are small enough
  to live fully inline in the header.
- Produces (used by every later task in this plan):
  ```cpp
  namespace ss::io::dawproject
  {
      class ArchiveWriter
      {
      public:
          void addXml   (const juce::String& storedPathName, const juce::XmlElement&);
          void addFile  (const juce::String& storedPathName, const juce::File& sourceFile);
          void addBytes (const juce::String& storedPathName, const juce::MemoryBlock&);
          bool writeTo  (const juce::File& target, juce::String& errorOut);
      private:
          juce::ZipFile::Builder builder;
      };

      class ArchiveReader
      {
      public:
          explicit ArchiveReader (const juce::File& source);
          std::unique_ptr<juce::XmlElement> readProjectXml (juce::String& errorOut) const;
          bool readEntry          (const juce::String& entryPath, juce::MemoryBlock& dataOut) const;
          bool extractEntryToFile (const juce::String& entryPath, const juce::File& targetFile) const;
      private:
          juce::ZipFile zip;
      };
  }
  ```

- [ ] **Step 1: Vendor the schema files, pinned by blob SHA**

```bash
mkdir -p "docs/superpowers/specs/dawproject-schema"
curl -s "https://raw.githubusercontent.com/bitwig/dawproject/main/Project.xsd" \
    -o "docs/superpowers/specs/dawproject-schema/Project.xsd"
curl -s "https://raw.githubusercontent.com/bitwig/dawproject/main/MetaData.xsd" \
    -o "docs/superpowers/specs/dawproject-schema/MetaData.xsd"
git hash-object "docs/superpowers/specs/dawproject-schema/Project.xsd"
git hash-object "docs/superpowers/specs/dawproject-schema/MetaData.xsd"
```

Expected: the two `git hash-object` lines print exactly
`ef4055976e0ea5615567c363a5c83a13e79b768c` and
`cc60f065821b54c7daac6ffbeabe93376329b0cf` (the blob SHAs pinned in the
spec's Tech Stack section). If either doesn't match, the upstream file
changed since the spec was written — stop and re-read the spec's Data
Model section against the new file before continuing to Task 2.

- [ ] **Step 2: Write the failing round-trip test**

Create `Source/IO/DawProjectTests.cpp`:

```cpp
#include "IO/DawProject.h"

namespace ss
{

class DawProjectUnitTests final : public juce::UnitTest
{
public:
    DawProjectUnitTests() : juce::UnitTest ("DawProject", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("ArchiveWriter/ArchiveReader round-trip an XML root and a byte entry");
        {
            juce::TemporaryFile temp (".dawproject");

            juce::XmlElement root ("Project");
            root.setAttribute ("version", "1.0");
            auto* app = root.createNewChildElement ("Application");
            app->setAttribute ("name", "KANADE DAW");
            app->setAttribute ("version", "0.2.0");

            const juce::MemoryBlock payload ("hello-audio-bytes", 17);

            io::dawproject::ArchiveWriter writer;
            writer.addXml ("project.xml", root);
            writer.addBytes ("audio/test.txt", payload);

            juce::String writeError;
            expect (writer.writeTo (temp.getFile(), writeError), writeError);

            io::dawproject::ArchiveReader reader (temp.getFile());

            juce::String readError;
            auto readRoot = reader.readProjectXml (readError);
            expect (readRoot != nullptr, readError);
            expectEquals (readRoot->getStringAttribute ("version"), juce::String ("1.0"));
            auto* readApp = readRoot->getChildByName ("Application");
            expect (readApp != nullptr);
            expectEquals (readApp->getStringAttribute ("name"), juce::String ("KANADE DAW"));

            juce::MemoryBlock readPayload;
            expect (reader.readEntry ("audio/test.txt", readPayload));
            expect (readPayload == payload);
        }

        beginTest ("ArchiveReader.readProjectXml fails cleanly on a non-zip file");
        {
            juce::TemporaryFile temp (".dawproject");
            temp.getFile().replaceWithText ("not a zip file at all");

            io::dawproject::ArchiveReader reader (temp.getFile());
            juce::String error;
            auto result = reader.readProjectXml (error);
            expect (result == nullptr);
            expect (error.isNotEmpty());
        }
    }
};

static DawProjectUnitTests dawProjectUnitTests;

}
```

- [ ] **Step 3: Run the test suite to confirm it fails to compile (the types don't exist yet)**

```bash
cmake --build build --config Debug > build_task1.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — compile error, `ss::io::dawproject::ArchiveWriter` (and
`DawProject.h`) do not exist yet.

- [ ] **Step 4: Implement `DawProject.h`**

```cpp
#pragma once
#include "Core/Project.h"
#include "Plugins/PluginManager.h"
#include <juce_core/juce_core.h>

namespace ss::io::dawproject
{
    /** Thin wrapper over juce::ZipFile::Builder so the export passes in
        later tasks (structure, clips, automation, plugin state, scenes)
        can each add their own entries independently, then write once. */
    class ArchiveWriter
    {
    public:
        void addXml (const juce::String& storedPathName, const juce::XmlElement& xml)
        {
            const auto text = xml.toString();
            addBytes (storedPathName, juce::MemoryBlock (text.toUTF8().getAddress(), text.getNumBytesAsUTF8()));
        }

        void addFile (const juce::String& storedPathName, const juce::File& sourceFile)
        {
            builder.addFile (sourceFile, 6, storedPathName);
        }

        void addBytes (const juce::String& storedPathName, const juce::MemoryBlock& data)
        {
            // keepInternalCopyOfData=true is load-bearing, not optional: ZipFile::Builder
            // doesn't read the stream until writeTo() runs, and every caller here (this
            // function, addXml, and every later task's plugin-state/audio-file writes)
            // routinely passes a temporary MemoryBlock that would otherwise be destroyed
            // long before that read happens - found as a real heap-use-after-free during
            // Task 1's review, not a hypothetical.
            builder.addEntry (new juce::MemoryInputStream (data, true), 6,
                              storedPathName, juce::Time::getCurrentTime());
        }

        bool writeTo (const juce::File& target, juce::String& errorOut)
        {
            juce::FileOutputStream out (target);
            if (! out.openedOk())
            {
                errorOut = "Could not open \"" + target.getFullPathName() + "\" for writing.";
                return false;
            }
            if (! builder.writeToStream (out, nullptr))
            {
                errorOut = "Failed to write the DAWproject archive.";
                return false;
            }
            return true;
        }

    private:
        juce::ZipFile::Builder builder;
    };

    /** Thin wrapper over juce::ZipFile for reading a .dawproject archive
        written by ArchiveWriter (or by another DAW). */
    class ArchiveReader
    {
    public:
        explicit ArchiveReader (const juce::File& source) : zip (source) {}

        std::unique_ptr<juce::XmlElement> readProjectXml (juce::String& errorOut) const
        {
            const auto index = zip.getIndexOfFileName ("project.xml");
            if (index < 0)
            {
                errorOut = "This file has no project.xml entry - it is not a valid .dawproject.";
                return nullptr;
            }

            std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            if (stream == nullptr)
            {
                errorOut = "Could not read the project.xml entry from the archive.";
                return nullptr;
            }

            auto xml = juce::parseXML (stream->readEntireStreamAsString());
            if (xml == nullptr)
            {
                errorOut = "project.xml did not parse as valid XML.";
                return nullptr;
            }
            return xml;
        }

        bool readEntry (const juce::String& entryPath, juce::MemoryBlock& dataOut) const
        {
            const auto index = zip.getIndexOfFileName (entryPath);
            if (index < 0)
                return false;

            std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            if (stream == nullptr)
                return false;

            dataOut.reset();
            stream->readIntoMemoryBlock (dataOut);
            return true;
        }

        bool extractEntryToFile (const juce::String& entryPath, const juce::File& targetFile) const
        {
            const auto index = zip.getIndexOfFileName (entryPath);
            if (index < 0)
                return false;

            std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            if (stream == nullptr)
                return false;

            targetFile.getParentDirectory().createDirectory();
            juce::FileOutputStream out (targetFile);
            if (! out.openedOk())
                return false;

            out.writeFromInputStream (*stream, -1);
            return true;
        }

    private:
        juce::ZipFile zip;
    };
}
```

Note: `ArchiveReader` intentionally has no `isValid()`/error state on
construction — `juce::ZipFile` degrades gracefully to zero entries for a
non-zip file, which `readProjectXml` already turns into a clean `errorOut`
("no project.xml entry"). That's why the second test in Step 2 works
without any extra plumbing.

- [ ] **Step 5: Run the test suite to confirm it passes**

```bash
cmake --build build --config Debug > build_task1.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task1.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task1.log
```

Expected: exit code 0 for the build, and the DawProject test lines show
0 failures.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/dawproject-schema/Project.xsd \
        docs/superpowers/specs/dawproject-schema/MetaData.xsd \
        Source/IO/DawProject.h Source/IO/DawProjectTests.cpp
git commit -m "Add DAWproject archive read/write plumbing"
```

---

### Task 2: Export — Structure (tracks, channels, buses, master, tempo, markers)

Confirmed against `docs/superpowers/specs/dawproject-schema/Project.xsd`
(vendored in Task 1): `<Structure>` holds one `<Track>` per mixer strip,
each with a nested `<Channel>` (never a `<Structure>`-level sibling — see
the spec's Risks section, resolved against the reference Java source).
`<Arrangement><Lanes timeUnit="beats">` holds per-track content
(`<Clips track="...">`, `<Points track="...">`) as flat, IDREF-scoped
siblings — Task 3 and Task 4 append into the same `<Lanes>` this task
creates, they do not nest inside `<Track>`.

**Files:**
- Create: `Source/IO/DawProject.cpp`
- Modify: `Source/IO/DawProject.h` (add function declarations + xml-id helpers)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: `ArchiveWriter`/`ArchiveReader` (Task 1, not used by this task's
  test directly, but `DawProject.cpp` now exists for them to live in).
- Produces (used by Tasks 3, 4, 5):
  ```cpp
  namespace ss::io::dawproject
  {
      juce::String trackXmlId       (TrackId id);
      juce::String channelXmlId     (TrackId id);
      juce::String busTrackXmlId    (int busId);
      juce::String busChannelXmlId  (int busId);
      extern const juce::String masterTrackXmlId;    // "track-master"
      extern const juce::String masterChannelXmlId;  // "channel-master"

      std::unique_ptr<juce::XmlElement> buildProjectSkeleton (const Project&, juce::StringArray& warningsOut);
  }
  ```
  The returned root always has, ready for later tasks to append into:
  `<Project><Application/><Transport/><Structure>...</Structure>
  <Arrangement><Lanes timeUnit="beats"/>[<Markers/>]</Arrangement>
  <Scenes/></Project>`.

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`, inside `runTest()` after the
Task 1 tests:

```cpp
        beginTest ("buildProjectSkeleton maps tracks, buses, master, tempo and markers");
        {
            Project project;
            project.tempo.setEvents ({ { 0.0, 140.0 } });
            project.tempo.setTimeSignatures ({ { 0.0, 3, 4 } });
            project.markers.push_back ({ 8.0, "Chorus" });

            auto& bus = project.addBus ("Drum Bus");

            auto& guitar = project.addTrack (TrackType::audio, "Guitar");
            guitar.gainDb = -3.0f;
            guitar.pan = -0.5f;
            guitar.muted = true;
            guitar.outputBus = bus.id;
            guitar.sends.push_back ({ bus.id, 0.25f });

            project.addTrack (TrackType::midi, "Synth");
            project.addTrack (TrackType::utau, "Vocal");

            juce::StringArray warnings;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);

            expect (root != nullptr);
            expectEquals (root->getStringAttribute ("version"), juce::String ("1.0"));

            auto* transport = root->getChildByName ("Transport");
            expect (transport != nullptr);
            expectWithinAbsoluteError (transport->getChildByName ("Tempo")->getDoubleAttribute ("value"), 140.0, 1.0e-6);
            auto* ts = transport->getChildByName ("TimeSignature");
            expectEquals (ts->getIntAttribute ("numerator"), 3);
            expectEquals (ts->getIntAttribute ("denominator"), 4);

            auto* structure = root->getChildByName ("Structure");
            expect (structure != nullptr);

            int numTracks = 0;
            for (auto* t : structure->getChildIterator())
                if (t->hasTagName ("Track")) ++numTracks;
            expectEquals (numTracks, 4);   // Guitar + Synth + Bus-track + Master (Vocal/utau skipped)

            auto* guitarTrack = structure->getChildByName ("Track");   // first <Track> == first non-utau KANADE track
            expectEquals (guitarTrack->getStringAttribute ("name"), juce::String ("Guitar"));
            expectEquals (guitarTrack->getStringAttribute ("contentType"), juce::String ("audio"));

            auto* guitarChannel = guitarTrack->getChildByName ("Channel");
            expect (guitarChannel != nullptr);
            expectWithinAbsoluteError (guitarChannel->getChildByName ("Volume")->getDoubleAttribute ("value"), -3.0, 1.0e-6);
            expectEquals (guitarChannel->getChildByName ("Mute")->getStringAttribute ("value"), juce::String ("true"));
            expectEquals (guitarChannel->getStringAttribute ("destination"), io::dawproject::busChannelXmlId (bus.id));

            auto* sends = guitarChannel->getChildByName ("Sends");
            expect (sends != nullptr);
            auto* send = sends->getChildByName ("Send");
            expect (send != nullptr);
            expectEquals (send->getStringAttribute ("type"), juce::String ("post"));

            expectEquals (warnings.size(), 1);
            expect (warnings[0].contains ("Vocal"));

            auto* markers = root->getChildByName ("Arrangement")->getChildByName ("Markers");
            expect (markers != nullptr);
            expectWithinAbsoluteError (markers->getChildByName ("Marker")->getDoubleAttribute ("time"), 8.0, 1.0e-6);
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task2.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — compile error, `buildProjectSkeleton` doesn't exist yet.

- [ ] **Step 3: Add the declarations to `Source/IO/DawProject.h`**

Add inside `namespace ss::io::dawproject { ... }`, after the
`ArchiveReader` class from Task 1:

```cpp
    juce::String trackXmlId      (TrackId id);
    juce::String channelXmlId    (TrackId id);
    juce::String busTrackXmlId   (int busId);
    juce::String busChannelXmlId (int busId);
    extern const juce::String masterTrackXmlId;
    extern const juce::String masterChannelXmlId;

    std::unique_ptr<juce::XmlElement> buildProjectSkeleton (const Project& project,
                                                             juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
#include "IO/DawProject.h"

namespace ss::io::dawproject
{
    juce::String trackXmlId      (TrackId id)  { return "track-"   + juce::String (id); }
    juce::String channelXmlId    (TrackId id)  { return "channel-" + juce::String (id); }
    juce::String busTrackXmlId   (int busId)   { return "bus-track-"   + juce::String (busId); }
    juce::String busChannelXmlId (int busId)   { return "bus-channel-" + juce::String (busId); }
    const juce::String masterTrackXmlId   = "track-master";
    const juce::String masterChannelXmlId = "channel-master";

    namespace
    {
        void setRealParam (juce::XmlElement& parent, const juce::String& tag,
                            const juce::String& unit, double value)
        {
            auto* p = parent.createNewChildElement (tag);
            p->setAttribute ("unit", unit);
            p->setAttribute ("value", value);
        }

        void setBoolParam (juce::XmlElement& parent, const juce::String& tag, bool value)
        {
            parent.createNewChildElement (tag)->setAttribute ("value", value ? "true" : "false");
        }

        void addChannel (juce::XmlElement& trackEl, const juce::String& channelId,
                          const juce::String& mixerRole, const juce::String& destinationId,
                          float gainDb, float pan, bool muted, bool soloed)
        {
            auto* channel = trackEl.createNewChildElement ("Channel");
            channel->setAttribute ("id", channelId);
            channel->setAttribute ("audioChannels", 2);
            channel->setAttribute ("role", mixerRole);
            channel->setAttribute ("solo", soloed ? "true" : "false");
            if (destinationId.isNotEmpty())
                channel->setAttribute ("destination", destinationId);

            setRealParam (*channel, "Volume", "decibel", (double) gainDb);
            setRealParam (*channel, "Pan",    "linear",  (double) pan);
            setBoolParam (*channel, "Mute", muted);
        }

        juce::XmlElement* addTrackElement (juce::XmlElement& structure, const juce::String& id,
                                            const juce::String& name, const juce::String& contentType)
        {
            auto* trackEl = structure.createNewChildElement ("Track");
            trackEl->setAttribute ("id", id);
            trackEl->setAttribute ("name", name);
            trackEl->setAttribute ("contentType", contentType);
            trackEl->setAttribute ("loaded", "true");
            return trackEl;
        }
    }

    std::unique_ptr<juce::XmlElement> buildProjectSkeleton (const Project& project,
                                                             juce::StringArray& warningsOut)
    {
        auto root = std::make_unique<juce::XmlElement> ("Project");
        root->setAttribute ("version", "1.0");

        auto* application = root->createNewChildElement ("Application");
        application->setAttribute ("name", "KANADE DAW");
        application->setAttribute ("version", JUCE_APPLICATION_VERSION_STRING);

        auto* transport = root->createNewChildElement ("Transport");
        setRealParam (*transport, "Tempo", "bpm", project.tempo.bpmAt (0.0));
        {
            const auto ts = project.tempo.timeSignatureAt (0.0);
            auto* tsEl = transport->createNewChildElement ("TimeSignature");
            tsEl->setAttribute ("numerator", ts.numerator);
            tsEl->setAttribute ("denominator", ts.denominator);
        }

        auto* structure = root->createNewChildElement ("Structure");

        for (const auto& trackPtr : project.getTracks())
        {
            const auto& track = *trackPtr;
            if (track.getType() == TrackType::utau)
            {
                warningsOut.add ("Skipped UTAU track \"" + track.name
                                  + "\" - no equivalent in DAWproject.");
                continue;
            }

            auto* trackEl = addTrackElement (*structure, trackXmlId (track.getId()), track.name,
                                             track.getType() == TrackType::audio ? "audio" : "notes");
            trackEl->setAttribute ("color", "#" + track.colour.toDisplayString (false));

            const auto destination = track.outputBus == 0 ? masterChannelXmlId
                                                            : busChannelXmlId (track.outputBus);
            addChannel (*trackEl, channelXmlId (track.getId()), "regular", destination,
                       track.gainDb, track.pan, track.muted, track.soloed);

            if (! track.sends.empty())
            {
                auto* sends = trackEl->getChildByName ("Channel")->createNewChildElement ("Sends");
                for (const auto& send : track.sends)
                {
                    auto* sendEl = sends->createNewChildElement ("Send");
                    sendEl->setAttribute ("destination", busChannelXmlId (send.busId));
                    sendEl->setAttribute ("type", "post");
                    setRealParam (*sendEl, "Volume", "linear", (double) send.level);
                }
            }
        }

        for (const auto& bus : project.buses)
        {
            auto* busTrackEl = addTrackElement (*structure, busTrackXmlId (bus.id), bus.name, "audio");
            addChannel (*busTrackEl, busChannelXmlId (bus.id), "submix", masterChannelXmlId,
                       bus.gainDb, bus.pan, bus.muted, false);
        }

        auto* masterTrackEl = addTrackElement (*structure, masterTrackXmlId, "Master", "audio");
        addChannel (*masterTrackEl, masterChannelXmlId, "master", juce::String(), 0.0f, 0.0f, false, false);

        auto* arrangement = root->createNewChildElement ("Arrangement");
        auto* lanes = arrangement->createNewChildElement ("Lanes");
        lanes->setAttribute ("timeUnit", "beats");

        if (! project.markers.empty())
        {
            auto* markers = arrangement->createNewChildElement ("Markers");
            for (const auto& marker : project.markers)
            {
                auto* markerEl = markers->createNewChildElement ("Marker");
                markerEl->setAttribute ("time", marker.beat);
                markerEl->setAttribute ("name", marker.name);
            }
        }

        root->createNewChildElement ("Scenes");

        return root;
    }
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task2.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task2.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task2.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Export DAWproject Structure: tracks, channels, buses, master, tempo, markers"
```

---

### Task 3: Export — Clips (audio + notes)

Per-clip gain has no direct attribute on DAWproject's `Clip` (confirmed in
the spec's Risks section against `timeline/Clip.java`): a `gainDb != 0`
clip wraps its content in a `<Lanes>` holding both the content and a
`<Points Target expression="gain">`. `playbackRate != 1.0` similarly wraps
the `<Audio>` in a `<Warps>` with exactly two `<Warp>` points. Both wrap
the *same* inner content element, and both can apply to the same clip
(order: `Audio` → optionally wrapped in `Warps` → optionally wrapped in
`Lanes`+`Points`).

**Note on two unverified conventions** (flagged, not guessed silently):
MIDI `vel` is written normalized (`velocity / 127.0`) rather than as the
raw 1..127 KANADE DAW value, and `channel` is written 0-based
(`note.channel - 1`) rather than KANADE DAW's 1-based. Both match near-universal
MIDI-adjacent serialization convention but were not directly confirmed
against a real exported sample file — if Task 10's real-plugin/real-DAW
manual verification pass shows notes importing at the wrong velocity or
channel in Studio One/Bitwig, flip these two conversions first.

**Files:**
- Modify: `Source/IO/DawProject.h` (add `addClips` declaration)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: `trackXmlId()` (Task 2), `ArchiveWriter::addFile()` (Task 1).
- Produces (used by Task 5's `exportDawProject` orchestrator):
  ```cpp
  namespace ss::io::dawproject
  {
      void addClips (juce::XmlElement& lanes, const Project&, juce::AudioFormatManager&,
                      ArchiveWriter&, juce::StringArray& warningsOut);
  }
  ```
  `lanes` is the `<Lanes>` element `buildProjectSkeleton` (Task 2) already
  created under `<Arrangement>` — this function appends `<Clips
  track="...">` elements into it, one per track that has audio or MIDI
  clips.

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("addClips writes audio and MIDI clips, with gain and playbackRate wrapping");
        {
            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 } });   // 1 beat == 0.5s at 120bpm

            juce::TemporaryFile wav (".wav");
            {
                juce::WavAudioFormat wavFormat;
                std::unique_ptr<juce::FileOutputStream> out (wav.getFile().createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wavFormat.createWriterFor (out.get(), 48000.0, 2, 16, {}, 0));
                expect (writer != nullptr);
                out.release();   // writer now owns the stream
                juce::AudioBuffer<float> silence (2, 4800);
                silence.clear();
                writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
            }

            auto& track = project.addTrack (TrackType::audio, "Guitar");
            auto& clip = track.audioClips.emplace_back();
            clip.id = project.nextClipId();
            clip.name = "Take 1";
            clip.sourceFile = wav.getFile();
            clip.startBeats = 4.0;
            clip.lengthBeats = 2.0;
            clip.gainDb = -6.0f;
            clip.playbackRate = 2.0;

            auto& midiTrack = project.addTrack (TrackType::midi, "Synth");
            auto& midiClip = midiTrack.midiClips.emplace_back();
            midiClip.id = project.nextClipId();
            midiClip.startBeats = 0.0;
            midiClip.lengthBeats = 4.0;
            midiClip.notes.push_back ({ 60, 0.0, 1.0, 127, 1, 1.0f });

            juce::StringArray warnings;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);
            auto* lanes = root->getChildByName ("Arrangement")->getChildByName ("Lanes");

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            io::dawproject::ArchiveWriter writer;
            io::dawproject::addClips (*lanes, project, formats, writer, warnings);

            juce::XmlElement* audioClipsEl = nullptr;
            juce::XmlElement* midiClipsEl  = nullptr;
            for (auto* el : lanes->getChildIterator())
            {
                if (! el->hasTagName ("Clips")) continue;
                if (el->getStringAttribute ("track") == io::dawproject::trackXmlId (track.getId()))
                    audioClipsEl = el;
                if (el->getStringAttribute ("track") == io::dawproject::trackXmlId (midiTrack.getId()))
                    midiClipsEl = el;
            }
            expect (audioClipsEl != nullptr);
            expect (midiClipsEl  != nullptr);

            auto* clipEl = audioClipsEl->getChildByName ("Clip");
            expect (clipEl != nullptr);
            expectWithinAbsoluteError (clipEl->getDoubleAttribute ("time"), 4.0, 1.0e-6);
            expectWithinAbsoluteError (clipEl->getDoubleAttribute ("duration"), 2.0, 1.0e-6);

            // gainDb != 0 -> wrapped in <Lanes><Points Target expression="gain">...
            auto* gainLanes = clipEl->getChildByName ("Lanes");
            expect (gainLanes != nullptr);
            auto* points = gainLanes->getChildByName ("Points");
            expect (points != nullptr);
            expectEquals (points->getChildByName ("Target")->getStringAttribute ("expression"), juce::String ("gain"));
            expectWithinAbsoluteError (points->getChildByName ("RealPoint")->getDoubleAttribute ("value"), -6.0, 1.0e-6);

            // playbackRate != 1 -> <Warps> nested inside that <Lanes>, wrapping <Audio>
            auto* warps = gainLanes->getChildByName ("Warps");
            expect (warps != nullptr);
            expect (warps->getChildByName ("Audio") != nullptr);
            expectEquals (warps->getChildByName ("Audio")->getIntAttribute ("channels"), 2);

            auto* midiClipEl = midiClipsEl->getChildByName ("Clip");
            expect (midiClipEl != nullptr);
            auto* notesEl = midiClipEl->getChildByName ("Notes");
            expect (notesEl != nullptr);
            auto* noteEl = notesEl->getChildByName ("Note");
            expect (noteEl != nullptr);
            expectEquals (noteEl->getIntAttribute ("key"), 60);
            expectEquals (noteEl->getIntAttribute ("channel"), 0);   // KANADE 1-based -> DAWproject 0-based

            expect (warnings.isEmpty());
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task3.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `addClips` doesn't exist yet.

- [ ] **Step 3: Add the declaration to `Source/IO/DawProject.h`**

```cpp
    void addClips (juce::XmlElement& lanes, const Project& project, juce::AudioFormatManager&,
                    ArchiveWriter& writer, juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        std::unique_ptr<juce::XmlElement> buildAudioElement (juce::AudioFormatReader& reader,
                                                               const juce::String& storedPath)
        {
            auto audio = std::make_unique<juce::XmlElement> ("Audio");
            audio->setAttribute ("channels", (int) reader.numChannels);
            audio->setAttribute ("sampleRate", (int) reader.sampleRate);
            audio->setAttribute ("duration", (double) reader.lengthInSamples / reader.sampleRate);
            audio->createNewChildElement ("File")->setAttribute ("path", storedPath);
            return audio;
        }

        std::unique_ptr<juce::XmlElement> wrapInGainLanesIfNeeded (std::unique_ptr<juce::XmlElement> content,
                                                                     float gainDb)
        {
            if (gainDb == 0.0f)
                return content;

            auto lanesWrap = std::make_unique<juce::XmlElement> ("Lanes");
            lanesWrap->addChildElement (content.release());
            auto* points = lanesWrap->createNewChildElement ("Points");
            points->setAttribute ("unit", "decibel");
            points->createNewChildElement ("Target")->setAttribute ("expression", "gain");
            auto* point = points->createNewChildElement ("RealPoint");
            point->setAttribute ("time", 0.0);
            point->setAttribute ("value", (double) gainDb);
            return lanesWrap;
        }

        std::unique_ptr<juce::XmlElement> wrapInWarpsIfNeeded (std::unique_ptr<juce::XmlElement> content,
                                                                 const AudioClip& clip, const TempoMap& tempo)
        {
            if (clip.playbackRate == 1.0)
                return content;

            const auto startSec = tempo.beatsToSeconds (clip.startBeats);
            const auto endSec   = tempo.beatsToSeconds (clip.endBeats());
            const auto contentSecondsConsumed = (endSec - startSec) * clip.playbackRate;

            auto warps = std::make_unique<juce::XmlElement> ("Warps");
            warps->setAttribute ("contentTimeUnit", "seconds");
            warps->addChildElement (content.release());
            auto* w0 = warps->createNewChildElement ("Warp");
            w0->setAttribute ("time", 0.0);
            w0->setAttribute ("contentTime", 0.0);
            auto* w1 = warps->createNewChildElement ("Warp");
            w1->setAttribute ("time", clip.lengthBeats);
            w1->setAttribute ("contentTime", contentSecondsConsumed);
            return warps;
        }

        void addAudioClip (juce::XmlElement& clipsEl, const AudioClip& clip, const TempoMap& tempo,
                            juce::AudioFormatManager& formats, ArchiveWriter& writer,
                            juce::StringArray& warningsOut)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (clip.sourceFile));
            if (reader == nullptr)
            {
                warningsOut.add ("Skipped audio clip \"" + clip.name + "\" - could not read \""
                                  + clip.sourceFile.getFullPathName() + "\".");
                return;
            }

            const auto storedPath = "audio/" + juce::String (clip.id) + "-" + clip.sourceFile.getFileName();
            writer.addFile (storedPath, clip.sourceFile);

            auto* clipEl = clipsEl.createNewChildElement ("Clip");
            clipEl->setAttribute ("time", clip.startBeats);
            clipEl->setAttribute ("duration", clip.lengthBeats);
            clipEl->setAttribute ("enable", "true");
            if (clip.name.isNotEmpty())
                clipEl->setAttribute ("name", clip.name);
            if (clip.fadeInSec > 0.0)
            {
                clipEl->setAttribute ("fadeInTime", clip.fadeInSec);
                clipEl->setAttribute ("fadeTimeUnit", "seconds");
            }
            if (clip.fadeOutSec > 0.0)
            {
                clipEl->setAttribute ("fadeOutTime", clip.fadeOutSec);
                clipEl->setAttribute ("fadeTimeUnit", "seconds");
            }

            std::unique_ptr<juce::XmlElement> content = buildAudioElement (*reader, storedPath);
            content = wrapInWarpsIfNeeded (std::move (content), clip, tempo);
            content = wrapInGainLanesIfNeeded (std::move (content), clip.gainDb);
            clipEl->addChildElement (content.release());
        }

        void addMidiClip (juce::XmlElement& clipsEl, const MidiClip& clip)
        {
            auto* clipEl = clipsEl.createNewChildElement ("Clip");
            clipEl->setAttribute ("time", clip.startBeats);
            clipEl->setAttribute ("duration", clip.lengthBeats);
            clipEl->setAttribute ("enable", "true");
            if (clip.name.isNotEmpty())
                clipEl->setAttribute ("name", clip.name);

            auto* notesEl = clipEl->createNewChildElement ("Notes");
            for (const auto& note : clip.notes)
            {
                auto* noteEl = notesEl->createNewChildElement ("Note");
                noteEl->setAttribute ("time", note.startBeats);
                noteEl->setAttribute ("duration", note.lengthBeats);
                noteEl->setAttribute ("key", note.pitch);
                noteEl->setAttribute ("channel", note.channel - 1);          // 1-based -> 0-based
                noteEl->setAttribute ("vel", (double) note.velocity / 127.0); // 1..127 -> 0..1
            }
        }
    }

    void addClips (juce::XmlElement& lanes, const Project& project, juce::AudioFormatManager& formats,
                    ArchiveWriter& writer, juce::StringArray& warningsOut)
    {
        for (const auto& trackPtr : project.getTracks())
        {
            const auto& track = *trackPtr;
            if (track.getType() == TrackType::utau)
                continue;   // already warned about in buildProjectSkeleton

            if (! track.audioClips.empty())
            {
                auto* clipsEl = lanes.createNewChildElement ("Clips");
                clipsEl->setAttribute ("track", trackXmlId (track.getId()));
                for (const auto& clip : track.audioClips)
                    addAudioClip (*clipsEl, clip, project.tempo, formats, writer, warningsOut);
            }

            if (! track.midiClips.empty())
            {
                auto* clipsEl = lanes.createNewChildElement ("Clips");
                clipsEl->setAttribute ("track", trackXmlId (track.getId()));
                for (const auto& clip : track.midiClips)
                    addMidiClip (*clipsEl, clip);
            }
        }
    }
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task3.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task3.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task3.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Export DAWproject clips: audio (gain/warp wrapping) and MIDI notes"
```

---

### Task 4: Export — Plugin state, builtin FX, and automation

Confirmed against `Source/Mixer/Mixer.cpp:826-871`: `Track::AutomationLane
::parameterId` uses exactly four shapes — `"gain"`, `"pan"`, `"mute"`,
`"fx:<slotIndex>:<paramId>"`, `"plugin:<slotIndex>:<paramIndex>"` — where
`slotIndex` indexes `Track::builtinFx`/`Track::plugins` by position and
`paramId` matches the literal ids `Source/Mixer/BuiltinFx.cpp` registers
via `addParam(...)` (e.g. `"threshold"`, `"ratio"`).

Devices and automation are built together, per track, in one pass —
automation for a plugin/FX parameter needs to point at that parameter's
XML `id`, and only parameters that are actually automated get declared at
all (KANADE DAW never has a loaded plugin instance during export, so it
can't enumerate a plugin's full parameter list — see spec Data Model).

**Individual sub-parameters with no DAWproject equivalent are omitted
silently** (e.g. compressor `knee`/`mix`, limiter `ceiling`/`lookahead`) —
`warningsOut` is reserved for whole skipped elements (a track, a clip, an
entire FX slot), not every parameter within an otherwise-represented
device. Forcing a parameter into a semantically wrong slot (e.g. `ceiling`
into `OutputGain`) would actively misrepresent the sound; an honest gap is
better than a wrong number.

**Files:**
- Modify: `Source/IO/DawProject.h` (add `addDevicesAndAutomation` declaration)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: `trackXmlId()`/`channelXmlId()` (Task 2), `PluginManager::
  findDescription()` (existing, `Source/Plugins/PluginManager.h:40`),
  `ArchiveWriter::addBytes()` (Task 1).
- Produces (used by Task 5's orchestrator):
  ```cpp
  namespace ss::io::dawproject
  {
      void addDevicesAndAutomation (juce::XmlElement& root, const Project&, PluginManager&,
                                     ArchiveWriter&, juce::StringArray& warningsOut);
  }
  ```
  `root` is the full tree from `buildProjectSkeleton` (Task 2) with clips
  already added (Task 3) — this function locates each track's `<Channel>`
  under `<Structure>` and appends `<Points track="...">` into
  `<Arrangement><Lanes>`, both by re-walking `root` rather than being
  handed sub-elements directly, since it needs both at once.

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("addDevicesAndAutomation maps plugin state, builtin FX, and automation lanes");
        {
            Project project;
            auto& track = project.addTrack (TrackType::audio, "Guitar");

            ss::PluginSlot plugin;
            plugin.identifier = "unknown-plugin-id";   // not installed - exercises the "loaded=false" path
            plugin.displayName = "Some Amp Sim";
            plugin.isInstrument = false;
            plugin.state.append ("fake-state-bytes", 16);
            track.plugins.push_back (plugin);

            ss::BuiltinFxSlot comp;
            comp.type = "compressor";
            comp.params.set ("threshold", -20.0);
            comp.params.set ("ratio", 4.0);
            comp.params.set ("attack", 10.0);
            comp.params.set ("release", 120.0);
            comp.params.set ("makeup", 2.0);
            comp.params.set ("knee", 6.0);
            comp.params.set ("mix", 1.0);
            track.builtinFx.push_back (comp);

            ss::BuiltinFxSlot verb;
            verb.type = "reverb";
            track.builtinFx.push_back (verb);

            track.automation.push_back ({ "gain", { { 0.0, -6.0f }, { 4.0, 0.0f } } });
            track.automation.push_back ({ "plugin:0:3", { { 0.0, 0.2f }, { 2.0, 0.8f } } });
            track.automation.push_back ({ "fx:0:threshold", { { 0.0, -30.0f } } });

            juce::StringArray warnings;
            Settings settings;
            PluginManager plugins (settings);
            io::dawproject::ArchiveWriter writer;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);
            io::dawproject::addDevicesAndAutomation (*root, project, plugins, writer, warnings);

            auto* channel = root->getChildByName ("Structure")->getChildByName ("Track")->getChildByName ("Channel");
            auto* devices = channel->getChildByName ("Devices");
            expect (devices != nullptr);

            auto* vst = devices->getChildByName ("Vst3Plugin");
            expect (vst != nullptr);
            expectEquals (vst->getStringAttribute ("loaded"), juce::String ("false"));
            expect (vst->getChildByName ("State") != nullptr);
            auto* pluginParams = vst->getChildByName ("Parameters");
            expect (pluginParams != nullptr);
            expectEquals (pluginParams->getChildByName ("RealParameter")->getIntAttribute ("parameterID"), 3);

            juce::XmlElement* compressorDevice = nullptr;
            for (auto* d : devices->getChildIterator())
                if (d->hasTagName ("BuiltinDevice") && d->getChildByName ("Compressor") != nullptr)
                    compressorDevice = d;
            expect (compressorDevice != nullptr);
            auto* compressorEl = compressorDevice->getChildByName ("Compressor");
            expectWithinAbsoluteError (compressorEl->getChildByName ("Threshold")->getDoubleAttribute ("value"), -20.0, 1.0e-6);
            expectWithinAbsoluteError (compressorEl->getChildByName ("OutputGain")->getDoubleAttribute ("value"), 2.0, 1.0e-6);
            expect (compressorEl->getChildByName ("Threshold")->getStringAttribute ("id").isNotEmpty());   // automated

            int numBuiltinDevices = 0;
            for (auto* d : devices->getChildIterator())
                if (d->hasTagName ("BuiltinDevice")) ++numBuiltinDevices;
            expectEquals (numBuiltinDevices, 1);   // reverb skipped
            expect (warnings.size() == 1 && warnings[0].contains ("reverb"));

            auto* lanes = root->getChildByName ("Arrangement")->getChildByName ("Lanes");
            int numPointsLanes = 0;
            for (auto* p : lanes->getChildIterator())
                if (p->hasTagName ("Points")) ++numPointsLanes;
            expectEquals (numPointsLanes, 3);   // gain, one plugin param, one fx param
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task4.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `addDevicesAndAutomation` doesn't exist yet.

- [ ] **Step 3: Add the declaration to `Source/IO/DawProject.h`**

```cpp
    void addDevicesAndAutomation (juce::XmlElement& root, const Project& project, PluginManager& plugins,
                                   ArchiveWriter& writer, juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        juce::XmlElement* findTrackElement (juce::XmlElement& structure, const juce::String& trackId)
        {
            for (auto* t : structure.getChildIterator())
                if (t->hasTagName ("Track") && t->getStringAttribute ("id") == trackId)
                    return t;
            return nullptr;
        }

        juce::String pluginDeviceXmlId (TrackId trackId, int slot)
        {
            return "track-" + juce::String (trackId) + "-plugin" + juce::String (slot);
        }
        juce::String pluginParamXmlId (TrackId trackId, int slot, int paramIndex)
        {
            return pluginDeviceXmlId (trackId, slot) + "-param" + juce::String (paramIndex);
        }
        juce::String builtinFxDeviceXmlId (TrackId trackId, int slot)
        {
            return "track-" + juce::String (trackId) + "-fx" + juce::String (slot);
        }
        juce::String builtinFxParamXmlId (TrackId trackId, int slot, const juce::String& paramId)
        {
            return builtinFxDeviceXmlId (trackId, slot) + "-" + paramId;
        }
        juce::String trackParamXmlId (TrackId trackId, const juce::String& which)
        {
            return channelXmlId (trackId) + "-" + which;
        }

        // Sets `tag`'s value attribute from fx.params[paramId], and (only if `automatedIds`
        // contains paramId) an `id` attribute so automation can target it.
        void addFxRealParam (juce::XmlElement& parent, const juce::String& tag, const juce::String& unit,
                              const ss::BuiltinFxSlot& fx, const juce::String& paramId,
                              TrackId trackId, int slot, const std::vector<juce::String>& automatedIds)
        {
            auto* p = parent.createNewChildElement (tag);
            p->setAttribute ("unit", unit);
            p->setAttribute ("value", (double) fx.params[juce::Identifier (paramId)]);
            if (std::find (automatedIds.begin(), automatedIds.end(), paramId) != automatedIds.end())
                p->setAttribute ("id", builtinFxParamXmlId (trackId, slot, paramId));
        }

        void addEqualizerParams (juce::XmlElement& eq, const ss::BuiltinFxSlot& fx,
                                  TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            struct Band { const char* type; const char* freqId; const char* gainId; const char* qId; const char* onId; int order; };
            static const Band bands[] = {
                { "highPass", "hpFreq",   nullptr,      nullptr,     "hpOn", 0 },
                { "lowShelf", "lowFreq",  "lowGain",    nullptr,     nullptr, 1 },
                { "bell",     "mid1Freq", "mid1Gain",   "mid1Q",     nullptr, 2 },
                { "bell",     "mid2Freq", "mid2Gain",   "mid2Q",     nullptr, 3 },
                { "highShelf","highFreq", "highGain",   nullptr,     nullptr, 4 },
                { "lowPass",  "lpFreq",   nullptr,      nullptr,     "lpOn", 5 },
            };
            for (const auto& band : bands)
            {
                auto* bandEl = eq.createNewChildElement ("Band");
                bandEl->setAttribute ("type", band.type);
                bandEl->setAttribute ("order", band.order);
                addFxRealParam (*bandEl, "Freq", "hertz", fx, band.freqId, trackId, slot, automated);
                if (band.gainId != nullptr)
                    addFxRealParam (*bandEl, "Gain", "decibel", fx, band.gainId, trackId, slot, automated);
                if (band.qId != nullptr)
                    addFxRealParam (*bandEl, "Q", "linear", fx, band.qId, trackId, slot, automated);
                if (band.onId != nullptr)
                {
                    const bool on = (double) fx.params[juce::Identifier (band.onId)] >= 0.5;
                    bandEl->createNewChildElement ("Enabled")->setAttribute ("value", on ? "true" : "false");
                }
            }
        }

        void addCompressorParams (juce::XmlElement& c, const ss::BuiltinFxSlot& fx,
                                   TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            addFxRealParam (c, "Threshold",   "decibel", fx, "threshold", trackId, slot, automated);
            addFxRealParam (c, "Ratio",       "linear",  fx, "ratio",     trackId, slot, automated);
            addFxRealParam (c, "Attack",      "seconds", fx, "attack",    trackId, slot, automated);
            addFxRealParam (c, "Release",     "seconds", fx, "release",   trackId, slot, automated);
            addFxRealParam (c, "OutputGain",  "decibel", fx, "makeup",    trackId, slot, automated);
            // knee/mix have no DAWproject compressor equivalent - omitted, see Task 4 note.
        }

        void addGateParams (juce::XmlElement& g, const ss::BuiltinFxSlot& fx,
                             TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            addFxRealParam (g, "Threshold", "decibel", fx, "threshold", trackId, slot, automated);
            addFxRealParam (g, "Range",     "decibel", fx, "range",     trackId, slot, automated);
            addFxRealParam (g, "Attack",    "seconds", fx, "attack",    trackId, slot, automated);
            addFxRealParam (g, "Release",   "seconds", fx, "release",   trackId, slot, automated);
            // hold has no DAWproject equivalent - omitted.
        }

        void addLimiterParams (juce::XmlElement& l, const ss::BuiltinFxSlot& fx,
                                TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            addFxRealParam (l, "InputGain", "decibel", fx, "input",   trackId, slot, automated);
            addFxRealParam (l, "Release",   "seconds", fx, "release", trackId, slot, automated);
            // ceiling/lookahead have no DAWproject equivalent - omitted rather than mismapped.
        }

        struct AutomationIndex
        {
            bool gain = false, pan = false, mute = false;
            std::map<int, std::vector<int>>          plugin;   // slot -> param indices
            std::map<int, std::vector<juce::String>> fx;       // slot -> param ids
        };

        AutomationIndex indexAutomation (const Track& track)
        {
            AutomationIndex idx;
            for (const auto& lane : track.automation)
            {
                if (lane.points.empty()) continue;
                if (lane.parameterId == "gain") idx.gain = true;
                else if (lane.parameterId == "pan") idx.pan = true;
                else if (lane.parameterId == "mute") idx.mute = true;
                else if (lane.parameterId.startsWith ("plugin:"))
                {
                    const auto body = lane.parameterId.substring (7);
                    idx.plugin[body.upToFirstOccurrenceOf (":", false, false).getIntValue()]
                        .push_back (body.fromFirstOccurrenceOf (":", false, false).getIntValue());
                }
                else if (lane.parameterId.startsWith ("fx:"))
                {
                    const auto body = lane.parameterId.substring (3);
                    idx.fx[body.upToFirstOccurrenceOf (":", false, false).getIntValue()]
                        .push_back (body.fromFirstOccurrenceOf (":", false, false));
                }
            }
            return idx;
        }

        void addPointsForLane (juce::XmlElement& lanes, const Track& track, const juce::String& laneParamId,
                                const juce::String& targetId, const juce::String& unit, bool isBool)
        {
            auto* points = lanes.createNewChildElement ("Points");
            points->setAttribute ("track", trackXmlId (track.getId()));
            points->setAttribute ("unit", unit);
            points->createNewChildElement ("Target")->setAttribute ("parameter", targetId);

            for (const auto& lane : track.automation)
            {
                if (lane.parameterId != laneParamId) continue;
                for (const auto& pt : lane.points)
                {
                    auto* pointEl = points->createNewChildElement (isBool ? "BoolPoint" : "RealPoint");
                    pointEl->setAttribute ("time", pt.first);
                    if (isBool) pointEl->setAttribute ("value", pt.second >= 0.5f ? "true" : "false");
                    else        pointEl->setAttribute ("value", (double) pt.second);
                }
            }
        }
    }

    void addDevicesAndAutomation (juce::XmlElement& root, const Project& project, PluginManager& plugins,
                                   ArchiveWriter& writer, juce::StringArray& warningsOut)
    {
        auto* structure = root.getChildByName ("Structure");
        auto* lanes = root.getChildByName ("Arrangement")->getChildByName ("Lanes");

        // Bus/master effect chains are not exported in this v1 pass - the mapping is
        // identical in principle (same addEqualizerParams-family helpers), but scoped out
        // to bound this plan's size (spec Scope). Never silent about it, though.
        for (const auto& bus : project.buses)
            if (! bus.builtinFx.empty())
                warningsOut.add ("Skipped the effect chain on bus \"" + bus.name
                                  + "\" - bus/master effect export isn't implemented yet.");
        if (! project.masterChain.empty())
            warningsOut.add ("Skipped the master bus effect chain - bus/master effect export isn't implemented yet.");

        for (const auto& trackPtr : project.getTracks())
        {
            const auto& track = *trackPtr;
            if (track.getType() == TrackType::utau)
                continue;

            auto* trackEl = findTrackElement (*structure, trackXmlId (track.getId()));
            auto* channel = trackEl->getChildByName ("Channel");
            const auto idx = indexAutomation (track);

            if (! track.plugins.empty())
            {
                auto* devices = channel->createNewChildElement ("Devices");
                for (int slot = 0; slot < (int) track.plugins.size(); ++slot)
                {
                    const auto& pluginSlot = track.plugins[(size_t) slot];
                    const auto* desc = plugins.findDescription (pluginSlot.identifier);

                    auto* device = devices->createNewChildElement ("Vst3Plugin");
                    device->setAttribute ("id", pluginDeviceXmlId (track.getId(), slot));
                    device->setAttribute ("deviceRole", pluginSlot.isInstrument ? "instrument" : "audioFX");
                    device->setAttribute ("deviceName", desc != nullptr ? desc->name : pluginSlot.displayName);
                    device->setAttribute ("deviceVendor", desc != nullptr ? desc->manufacturerName : juce::String());
                    device->setAttribute ("loaded", desc != nullptr ? "true" : "false");
                    device->createNewChildElement ("Enabled")->setAttribute ("value", pluginSlot.bypassed ? "false" : "true");

                    if (pluginSlot.state.getSize() > 0)
                    {
                        const auto stateName = "plugins/" + juce::Uuid().toString() + ".vst3-preset";
                        writer.addBytes (stateName, pluginSlot.state);
                        device->createNewChildElement ("State")->setAttribute ("path", stateName);
                    }

                    auto it = idx.plugin.find (slot);
                    if (it != idx.plugin.end())
                    {
                        auto* params = device->createNewChildElement ("Parameters");
                        for (int paramIndex : it->second)
                        {
                            auto* p = params->createNewChildElement ("RealParameter");
                            p->setAttribute ("id", pluginParamXmlId (track.getId(), slot, paramIndex));
                            p->setAttribute ("parameterID", paramIndex);
                            p->setAttribute ("unit", "normalized");
                        }
                    }
                }
            }

            for (int slot = 0; slot < (int) track.builtinFx.size(); ++slot)
            {
                const auto& fx = track.builtinFx[(size_t) slot];
                if (fx.type != "eq" && fx.type != "compressor" && fx.type != "gate" && fx.type != "limiter")
                {
                    warningsOut.add ("Skipped \"" + fx.type + "\" effect on track \"" + track.name
                                      + "\" - DAWproject has no built-in device for it.");
                    continue;
                }

                auto* devices = channel->getChildByName ("Devices");
                if (devices == nullptr)
                    devices = channel->createNewChildElement ("Devices");

                auto* builtinDevice = devices->createNewChildElement ("BuiltinDevice");
                builtinDevice->setAttribute ("id", builtinFxDeviceXmlId (track.getId(), slot));
                builtinDevice->setAttribute ("deviceRole", "audioFX");
                builtinDevice->createNewChildElement ("Enabled")->setAttribute ("value", fx.bypassed ? "false" : "true");

                const auto automatedIds = idx.fx.count (slot) ? idx.fx.at (slot) : std::vector<juce::String>{};

                if (fx.type == "eq")
                    addEqualizerParams (*builtinDevice->createNewChildElement ("Equalizer"), fx, track.getId(), slot, automatedIds);
                else if (fx.type == "compressor")
                    addCompressorParams (*builtinDevice->createNewChildElement ("Compressor"), fx, track.getId(), slot, automatedIds);
                else if (fx.type == "gate")
                    addGateParams (*builtinDevice->createNewChildElement ("NoiseGate"), fx, track.getId(), slot, automatedIds);
                else
                    addLimiterParams (*builtinDevice->createNewChildElement ("Limiter"), fx, track.getId(), slot, automatedIds);
            }

            // Every KANADE DAW automation value - gain, pan, mute, builtin FX, plugin - is
            // already stored as a 0..1 normalized breakpoint (confirmed against
            // Source/Mixer/Mixer.cpp:961-973: gain becomes -60+66*v, pan becomes v*2-1,
            // mute is v>=0.5, builtinFx/plugin call setParameterNormalised/setValue(v)
            // directly). Writing unit="normalized" everywhere means no denormalization
            // step anywhere in this function, not just for plugin parameters.
            if (idx.gain)
            {
                const auto id = trackParamXmlId (track.getId(), "gain");
                channel->getChildByName ("Volume")->setAttribute ("id", id);
                addPointsForLane (*lanes, track, "gain", id, "normalized", false);
            }
            if (idx.pan)
            {
                const auto id = trackParamXmlId (track.getId(), "pan");
                channel->getChildByName ("Pan")->setAttribute ("id", id);
                addPointsForLane (*lanes, track, "pan", id, "normalized", false);
            }
            if (idx.mute)
            {
                const auto id = trackParamXmlId (track.getId(), "mute");
                channel->getChildByName ("Mute")->setAttribute ("id", id);
                addPointsForLane (*lanes, track, "mute", id, "normalized", true);
            }
            for (const auto& [slot, indices] : idx.plugin)
                for (int paramIndex : indices)
                    addPointsForLane (*lanes, track, "plugin:" + juce::String (slot) + ":" + juce::String (paramIndex),
                                      pluginParamXmlId (track.getId(), slot, paramIndex), "normalized", false);
            for (const auto& [slot, ids] : idx.fx)
            {
                if (slot < 0 || slot >= (int) track.builtinFx.size())
                    continue;
                const auto& fxType = track.builtinFx[(size_t) slot].type;
                for (const auto& paramId : ids)
                {
                    // A user can automate ANY builtin-FX parameter via the generic
                    // automation-lane menu (TimelineView.cpp), including the ones this
                    // task deliberately has no DAWproject element for (hpOn/lpOn/knee/
                    // mix/hold/ceiling/lookahead - see the per-type dictionaries above).
                    // Emitting a Target IDREF for one of those would point at an id that
                    // exists nowhere in the document - structurally invalid output. Found
                    // as a real Important-severity bug during Task 4's review, not a
                    // hypothetical: skip silently, consistent with this task's existing
                    // "individual sub-parameter gaps are silent" policy for the static
                    // value itself.
                    if (! isKnownFxParam (fxType, paramId))
                        continue;
                    addPointsForLane (*lanes, track, "fx:" + juce::String (slot) + ":" + paramId,
                                      builtinFxParamXmlId (track.getId(), slot, paramId), "normalized", false);
                }
            }
        }
    }
```

`isKnownFxParam` (add alongside the other small helpers, e.g. near `addFxRealParam`):
```cpp
        bool isKnownFxParam (const juce::String& fxType, const juce::String& paramId)
        {
            static const std::map<juce::String, std::set<juce::String>> known {
                { "eq",         { "hpFreq", "lowFreq", "lowGain", "mid1Freq", "mid1Gain", "mid1Q",
                                  "mid2Freq", "mid2Gain", "mid2Q", "highFreq", "highGain", "lpFreq" } },
                { "compressor", { "threshold", "ratio", "attack", "release", "makeup" } },
                { "gate",       { "threshold", "range", "attack", "release" } },
                { "limiter",    { "input", "release" } },
            };
            const auto it = known.find (fxType);
            return it != known.end() && it->second.count (paramId) > 0;
        }
```
(`hpOn`/`lpOn` are deliberately excluded — `addEqualizerParams`'s on/off branch writes a static `<Enabled>` value but never assigns it an `id`, so there is nothing for an automation `Target` to point at. Making EQ band on/off separately automatable — a `BoolPoint` targeting `<Enabled>`, which the schema would technically allow — is a valid follow-up, not part of this fix.)

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task4.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task4.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task4.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Export DAWproject devices (plugin state, builtin FX) and automation"
```

---

### Task 5: Export — Scenes, MetaData.xml, and the `exportDawProject` orchestrator

This is the integration task: it wires Tasks 1-4's building blocks into
the one public function `File > Export` will call (Task 9 UI wiring). It
also covers `Project::scenes`/`Track::sessionSlots` — confirmed against
`Scene.java`'s own doc comment (spec Risks, resolved): one `<ClipSlot
track="...">` per track that has a session clip in that scene, all
grouped under `<Scene><Lanes>`.

**Files:**
- Modify: `Source/IO/DawProject.h` (add `addScenes` + the public
  `exportDawProject` declaration)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: everything from Tasks 1-4 (`ArchiveWriter`, `trackXmlId`,
  `wrapInGainLanesIfNeeded`, `buildAudioElement` — the last two move from
  anonymous-namespace-local in Task 3 to file-local `static` functions
  visible to this task's new code in the same `.cpp`, no header change).
- Produces (used by Task 9's UI wiring and Task 10's full round-trip test):
  ```cpp
  namespace ss::io
  {
      bool exportDawProject (const juce::File& target, const Project&, PluginManager&,
                              juce::String& errorOut, juce::StringArray& warningsOut);
  }
  ```

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("exportDawProject writes a readable archive with structure, clips and a scene");
        {
            Project project;
            project.name = "Demo Song";
            auto& scene = project.addScene ("Verse");

            auto& track = project.addTrack (TrackType::midi, "Synth");
            SessionClip sc;
            sc.kind = SessionClip::Kind::midi;
            sc.name = "Riff";
            sc.lengthBeats = 4.0;
            sc.notes.push_back ({ 67, 0.0, 1.0, 100, 1, 1.0f });
            track.setSessionClip (scene.id, sc);

            juce::TemporaryFile temp (".dawproject");
            juce::String error;
            juce::StringArray warnings;
            Settings settings;
            PluginManager plugins (settings);

            const bool ok = io::exportDawProject (temp.getFile(), project, plugins, error, warnings);
            expect (ok, error);
            expect (temp.getFile().existsAsFile());

            io::dawproject::ArchiveReader reader (temp.getFile());
            juce::String readError;
            auto root = reader.readProjectXml (readError);
            expect (root != nullptr, readError);
            expectEquals (root->getChildByName ("Application")->getStringAttribute ("name"), juce::String ("KANADE DAW"));

            juce::MemoryBlock metaBytes;
            expect (reader.readEntry ("metadata.xml", metaBytes));
            auto meta = juce::parseXML (metaBytes.toString());
            expect (meta != nullptr);
            expectEquals (meta->getChildByName ("Title")->getAllSubText(), juce::String ("Demo Song"));

            auto* scenesEl = root->getChildByName ("Scenes");
            expect (scenesEl != nullptr);
            auto* sceneEl = scenesEl->getChildByName ("Scene");
            expect (sceneEl != nullptr);
            auto* slot = sceneEl->getChildByName ("Lanes")->getChildByName ("ClipSlot");
            expect (slot != nullptr);
            expectEquals (slot->getStringAttribute ("track"), io::dawproject::trackXmlId (track.getId()));
            expect (slot->getChildByName ("Clip")->getChildByName ("Notes") != nullptr);
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task5.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `exportDawProject` doesn't exist yet.

- [ ] **Step 3: Add the declarations**

In `Source/IO/DawProject.h`, inside `namespace ss::io::dawproject`:

```cpp
    void addScenes (juce::XmlElement& root, const Project& project, juce::AudioFormatManager&,
                     ArchiveWriter& writer, juce::StringArray& warningsOut);
```

And, outside that namespace, in `namespace ss::io` (the same level as the
existing `exportMidiFile`/`exportMusicXml` in `FileIO.h` — add this to
`DawProject.h` directly, it does not need to live in `FileIO.h`):

```cpp
namespace ss::io
{
    bool exportDawProject (const juce::File& target, const Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut);
}
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        juce::String sceneXmlId (SceneId id) { return "scene-" + juce::String (id); }

        // Builds the clip's content FIRST and only creates <ClipSlot>/<Clip> once that
        // succeeds - mirrors addAudioClip's shape (Task 3) exactly. An unreadable audio
        // source is a TRUE skip (nothing appended to sceneLanes), not a childless shell -
        // found as a real bug during Task 5's review, where the original version created
        // the wrapper elements before checking readability. `tempo` is required to convert
        // sc.lengthBeats into the seconds a Warp's contentTime attribute needs - also found
        // missing during review (the original wrote raw beats into a seconds-typed field).
        void addSessionClip (juce::XmlElement& sceneLanes, const juce::String& trackXmlIdStr,
                              const SessionClip& sc, TrackId trackId, SceneId sceneId,
                              const TempoMap& tempo, juce::AudioFormatManager& formats,
                              ArchiveWriter& writer, juce::StringArray& warningsOut)
        {
            std::unique_ptr<juce::XmlElement> content;

            if (sc.kind == SessionClip::Kind::audio)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (sc.sourceFile));
                if (reader == nullptr)
                {
                    warningsOut.add ("Skipped session clip \"" + sc.name + "\" - could not read \""
                                      + sc.sourceFile.getFullPathName() + "\".");
                    return;   // nothing created - a true skip
                }
                const auto storedPath = "audio/scene-" + juce::String (sceneId) + "-track-"
                                         + juce::String (trackId) + "-" + sc.sourceFile.getFileName();
                writer.addFile (storedPath, sc.sourceFile);
                content = buildAudioElement (*reader, storedPath);

                if (sc.playbackRate != 1.0)
                {
                    auto warps = std::make_unique<juce::XmlElement> ("Warps");
                    warps->setAttribute ("contentTimeUnit", "seconds");
                    warps->addChildElement (content.release());
                    auto* w0 = warps->createNewChildElement ("Warp");
                    w0->setAttribute ("time", 0.0);
                    w0->setAttribute ("contentTime", 0.0);
                    auto* w1 = warps->createNewChildElement ("Warp");
                    w1->setAttribute ("time", sc.lengthBeats);
                    w1->setAttribute ("contentTime", tempo.beatsToSeconds (sc.lengthBeats) * sc.playbackRate);
                    content = std::move (warps);
                }
            }
            else
            {
                auto notesRoot = std::make_unique<juce::XmlElement> ("Notes");
                for (const auto& note : sc.notes)
                {
                    auto* n = notesRoot->createNewChildElement ("Note");
                    n->setAttribute ("time", note.startBeats);
                    n->setAttribute ("duration", note.lengthBeats);
                    n->setAttribute ("key", note.pitch);
                    n->setAttribute ("channel", note.channel - 1);
                    n->setAttribute ("vel", (double) note.velocity / 127.0);
                }
                content = std::move (notesRoot);
            }

            content = wrapInGainLanesIfNeeded (std::move (content), sc.gainDb);

            auto* slotEl = sceneLanes.createNewChildElement ("ClipSlot");
            slotEl->setAttribute ("track", trackXmlIdStr);
            auto* clipEl = slotEl->createNewChildElement ("Clip");
            clipEl->setAttribute ("time", 0.0);
            clipEl->setAttribute ("duration", sc.lengthBeats);
            if (sc.name.isNotEmpty())
                clipEl->setAttribute ("name", sc.name);
            clipEl->addChildElement (content.release());
        }
    }

    void addScenes (juce::XmlElement& root, const Project& project, juce::AudioFormatManager& formats,
                     ArchiveWriter& writer, juce::StringArray& warningsOut)
    {
        if (project.scenes.empty())
            return;

        auto* scenesEl = root.getChildByName ("Scenes");
        for (const auto& scene : project.scenes)
        {
            auto* sceneEl = scenesEl->createNewChildElement ("Scene");
            sceneEl->setAttribute ("id", sceneXmlId (scene.id));
            sceneEl->setAttribute ("name", scene.name);
            auto* sceneLanes = sceneEl->createNewChildElement ("Lanes");

            for (const auto& trackPtr : project.getTracks())
            {
                const auto& track = *trackPtr;
                if (track.getType() == TrackType::utau)
                    continue;

                const auto it = track.sessionSlots.find (scene.id);
                if (it == track.sessionSlots.end())
                    continue;

                addSessionClip (*sceneLanes, trackXmlId (track.getId()), it->second, track.getId(), scene.id,
                                project.tempo, formats, writer, warningsOut);
            }
        }
    }
}

namespace ss::io
{
    bool exportDawProject (const juce::File& target, const Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto root = dawproject::buildProjectSkeleton (project, warningsOut);
        auto* lanes = root->getChildByName ("Arrangement")->getChildByName ("Lanes");

        dawproject::ArchiveWriter writer;
        dawproject::addClips (*lanes, project, formats, writer, warningsOut);
        dawproject::addDevicesAndAutomation (*root, project, plugins, writer, warningsOut);
        dawproject::addScenes (*root, project, formats, writer, warningsOut);

        juce::XmlElement meta ("MetaData");
        if (project.name.isNotEmpty())
            meta.createNewChildElement ("Title")->addTextElement (project.name);

        writer.addXml ("project.xml", *root);
        writer.addXml ("metadata.xml", meta);

        return writer.writeTo (target, errorOut);
    }
}
```

Note: `DawProject.cpp` now has three `namespace ss::io::dawproject { ... }`
blocks across Tasks 2-5 plus a final `namespace ss::io { ... }` block for
the public entry point — that's normal for a `.cpp` built up task-by-task;
no need to consolidate them into one block.

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task5.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task5.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task5.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Wire up exportDawProject: scenes, metadata, end-to-end archive write"
```

---

### Task 6: Import — Structure (tracks, channels, buses, master, tempo)

Import must not assume the imported file's `<Track id="...">` strings
follow KANADE DAW's own `trackXmlId()` convention — a real Bitwig/Studio
One file uses opaque ids like `"id2"`. Every track/bus gets a *fresh*
KANADE `TrackId`/`Bus::id` via `project.addTrack`/`addBus` (which assign
their own ids internally), and this task builds the `file xml id -> fresh
KANADE id` maps that Tasks 7 and 8 need to resolve `track`/`parameter`
IDREFs elsewhere in the same file.

Two passes over `<Structure>` are required: a `destination` IDREF on one
track's `<Channel>` may point at a bus that appears later in the file, so
every Track/Bus object must exist before any `destination`/`Send` is
resolved.

**Files:**
- Modify: `Source/IO/DawProject.h` (add `ImportIds` + `parseStructure`)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: nothing from earlier tasks (parallel structure to Task 2, not
  built on it — import and export share no code, only the XML shape).
- Produces (used by Tasks 7, 8, 9):
  ```cpp
  namespace ss::io::dawproject
  {
      struct ImportIds
      {
          std::map<juce::String, TrackId> trackByXmlId;        // regular tracks only
          std::map<juce::String, int>     busIdByChannelXmlId; // bus's Channel xml id -> Bus::id
          juce::String                    masterChannelXmlId;  // "" if the file had no master Track
      };

      ImportIds parseStructure (const juce::XmlElement& root, Project& project, juce::StringArray& warningsOut);
  }
  ```
  `project` does not need to be empty — like the existing `importMidiFile`
  (`Source/IO/FileIO.cpp`), this *adds* tracks to whatever the caller
  already has open rather than requiring a fresh project. `Project` has
  no copy or move (`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`, a
  `juce::ChangeBroadcaster` base, a `juce::UndoManager` member), so Task
  9's "never partially mutate on failure" guarantee is achieved
  differently than a scratch-object swap: the only step that can fail
  (`ArchiveReader::readProjectXml`) always runs *before* any of
  `parseStructure`/`parseClips`/etc. touch `project` at all — see Task 9.

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("parseStructure rebuilds tracks, a bus, routing, sends and tempo from XML");
        {
            const auto xml = juce::String (R"XML(
<Project version="1.0">
  <Application name="Bitwig Studio" version="5.0.9"/>
  <Transport>
    <Tempo unit="bpm" value="128"/>
    <TimeSignature numerator="7" denominator="8"/>
  </Transport>
  <Structure>
    <Track id="idGuitar" name="Guitar" contentType="audio" color="#ff0000">
      <Channel id="chGuitar" role="regular" destination="chBus" solo="false">
        <Volume unit="decibel" value="-4.5"/>
        <Pan unit="linear" value="0.25"/>
        <Mute value="true"/>
        <Sends>
          <Send destination="chBus" type="post"><Volume unit="linear" value="0.6"/></Send>
        </Sends>
      </Channel>
    </Track>
    <Track id="idBus" name="Drum Bus" contentType="audio">
      <Channel id="chBus" role="submix" destination="chMaster"/>
    </Track>
    <Track id="idMaster" name="Master" contentType="audio">
      <Channel id="chMaster" role="master"/>
    </Track>
  </Structure>
</Project>
)XML");
            auto root = juce::parseXML (xml);
            expect (root != nullptr);

            Project project;
            juce::StringArray warnings;
            auto ids = io::dawproject::parseStructure (*root, project, warnings);

            expectWithinAbsoluteError (project.tempo.bpmAt (0.0), 128.0, 1.0e-6);
            const auto ts = project.tempo.timeSignatureAt (0.0);
            expectEquals (ts.numerator, 7);
            expectEquals (ts.denominator, 8);

            expectEquals (project.getNumTracks(), 1);   // bus and master are not regular Tracks
            expectEquals ((int) project.buses.size(), 1);

            auto& guitar = project.getTrack (0);
            expectEquals (guitar.name, juce::String ("Guitar"));
            expectWithinAbsoluteError (guitar.gainDb, -4.5f, 1.0e-4f);
            expectWithinAbsoluteError (guitar.pan, 0.25f, 1.0e-4f);
            expect (guitar.muted);
            expectEquals (guitar.outputBus, project.buses[0].id);
            expect (! guitar.sends.empty());
            expectEquals (guitar.sends[0].busId, project.buses[0].id);
            expectWithinAbsoluteError (guitar.sends[0].level, 0.6f, 1.0e-4f);

            expect (ids.trackByXmlId.count ("idGuitar") == 1);
            expectEquals (ids.trackByXmlId.at ("idGuitar"), guitar.getId());
            expect (ids.busIdByChannelXmlId.count ("chBus") == 1);
            expectEquals (ids.masterChannelXmlId, juce::String ("chMaster"));
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task6.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `parseStructure`/`ImportIds` don't exist yet.

- [ ] **Step 3: Add the declarations to `Source/IO/DawProject.h`**

```cpp
    struct ImportIds
    {
        std::map<juce::String, TrackId> trackByXmlId;
        std::map<juce::String, int>     busIdByChannelXmlId;
        juce::String                    masterChannelXmlId;
    };

    ImportIds parseStructure (const juce::XmlElement& root, Project& project, juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        TrackType contentTypeToTrackType (const juce::String& contentType)
        {
            return contentType.trim().startsWith ("notes") ? TrackType::midi : TrackType::audio;
        }

        double xmlRealParam (const juce::XmlElement& parent, const juce::String& tag, double defaultValue)
        {
            auto* el = parent.getChildByName (tag);
            return el != nullptr ? el->getDoubleAttribute ("value", defaultValue) : defaultValue;
        }

        bool xmlBoolParam (const juce::XmlElement& parent, const juce::String& tag, bool defaultValue)
        {
            auto* el = parent.getChildByName (tag);
            return el != nullptr ? el->getBoolAttribute ("value", defaultValue) : defaultValue;
        }
    }

    ImportIds parseStructure (const juce::XmlElement& root, Project& project, juce::StringArray& warningsOut)
    {
        ImportIds ids;

        if (auto* transport = root.getChildByName ("Transport"))
        {
            project.tempo.setEvents ({ { 0.0, xmlRealParam (*transport, "Tempo", 120.0) } });
            if (auto* ts = transport->getChildByName ("TimeSignature"))
                project.tempo.setTimeSignatures ({ { 0.0, ts->getIntAttribute ("numerator", 4),
                                                           ts->getIntAttribute ("denominator", 4) } });
        }

        // Export (buildProjectSkeleton, Task 2) always writes <Arrangement><Markers> when
        // project.markers is non-empty, but this function never read it back - a real gap
        // found during Task 10's full round-trip test (which crashed indexing an empty
        // project.markers vector, since nothing had ever populated it), invisible to every
        // earlier per-task test because none of them happened to include a marker in their
        // fixture. Markers are Project-level, not per-track, so they belong here rather
        // than in the Structure-iteration loop below.
        if (auto* arrangement = root.getChildByName ("Arrangement"))
            if (auto* markersEl = arrangement->getChildByName ("Markers"))
                for (auto* markerEl : markersEl->getChildIterator())
                    if (markerEl->hasTagName ("Marker"))
                        project.markers.push_back ({ markerEl->getDoubleAttribute ("time"),
                                                      markerEl->getStringAttribute ("name") });

        auto* structure = root.getChildByName ("Structure");
        if (structure == nullptr)
            return ids;

        std::map<juce::String, juce::String>            destinationByTrackXmlId;
        std::vector<std::tuple<Track*, size_t, juce::String>> pendingSends;   // track, sends-index, destination channel xml id

        for (auto* trackEl : structure->getChildIterator())
        {
            if (! trackEl->hasTagName ("Track"))
                continue;

            auto* channel = trackEl->getChildByName ("Channel");
            if (channel == nullptr)
            {
                warningsOut.add ("Skipped track \"" + trackEl->getStringAttribute ("name")
                                  + "\" - it has no <Channel>.");
                continue;
            }

            const auto role  = channel->getStringAttribute ("role", "regular");
            const auto xmlId = trackEl->getStringAttribute ("id");

            if (role == "master")
            {
                ids.masterChannelXmlId = channel->getStringAttribute ("id");
                continue;
            }

            if (role == "submix")
            {
                auto& bus = project.addBus (trackEl->getStringAttribute ("name", "Bus"));
                bus.gainDb = (float) xmlRealParam (*channel, "Volume", 0.0);
                bus.pan    = (float) xmlRealParam (*channel, "Pan", 0.0);
                bus.muted  = xmlBoolParam (*channel, "Mute", false);
                ids.busIdByChannelXmlId[channel->getStringAttribute ("id")] = bus.id;
                destinationByTrackXmlId[xmlId] = channel->getStringAttribute ("destination");
                continue;
            }

            auto& track = project.addTrack (contentTypeToTrackType (trackEl->getStringAttribute ("contentType", "audio")),
                                            trackEl->getStringAttribute ("name", "Track"));
            track.gainDb = (float) xmlRealParam (*channel, "Volume", 0.0);
            track.pan    = (float) xmlRealParam (*channel, "Pan", 0.0);
            track.muted  = xmlBoolParam (*channel, "Mute", false);
            track.soloed = channel->getBoolAttribute ("solo", false);
            if (trackEl->hasAttribute ("color"))
                // "ff" + is load-bearing, not decoration: Colour::fromString on a bare
                // 6-digit RGB string (what the export side writes) parses with no alpha
                // byte, producing alpha=0x00 (fully transparent) per JUCE's HexParser -
                // found as a real bug during Task 6's review (TimelineView.cpp paints
                // track.colour directly with no alpha override, so every imported track's
                // colour strip would render invisible without this).
                track.colour = juce::Colour::fromString ("ff" + trackEl->getStringAttribute ("color").removeCharacters ("#"));

            ids.trackByXmlId[xmlId] = track.getId();
            destinationByTrackXmlId[xmlId] = channel->getStringAttribute ("destination");

            if (auto* sends = channel->getChildByName ("Sends"))
                for (auto* sendEl : sends->getChildIterator())
                {
                    if (! sendEl->hasTagName ("Send"))
                        continue;
                    Track::Send send;
                    send.level = (float) xmlRealParam (*sendEl, "Volume", 0.0);
                    track.sends.push_back (send);
                    pendingSends.emplace_back (&track, track.sends.size() - 1, sendEl->getStringAttribute ("destination"));
                }
        }

        // Pass 2: every Track/Bus now has a KANADE id - resolve destination/Send IDREFs.
        for (auto& [xmlId, kanadeId] : ids.trackByXmlId)
        {
            auto* track = project.findTrack (kanadeId);
            if (track == nullptr) continue;
            const auto dest = destinationByTrackXmlId[xmlId];
            if (dest.isNotEmpty() && ids.busIdByChannelXmlId.count (dest))
                track->outputBus = ids.busIdByChannelXmlId.at (dest);
        }
        for (auto& [track, sendIndex, destXmlId] : pendingSends)
            if (ids.busIdByChannelXmlId.count (destXmlId))
                track->sends[sendIndex].busId = ids.busIdByChannelXmlId.at (destXmlId);

        return ids;
    }
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task6.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task6.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task6.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Import DAWproject Structure: tracks, channels, buses, routing, sends, tempo"
```

---

### Task 7: Import — Clips (audio + notes)

Mirrors Task 3 in reverse: peels off the optional `<Lanes>` (gain) and
`<Warps>` (playbackRate) wrappers Task 3 may have added, down to the base
`<Audio>` or `<Notes>` element. A foreign file's `<Warps>` may have more
than two `<Warp>` points (a real time-stretch map); per the spec's Scope
section this collapses to a single average rate from the first and last
points — a deliberate, documented simplification, not an oversight.

**Files:**
- Modify: `Source/IO/DawProject.h` (add `parseClips` declaration)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: `ImportIds` (Task 6), `ArchiveReader::extractEntryToFile()`
  (Task 1), `Project::getMediaFolder()`/`nextClipId()` (existing,
  `Source/Core/Project.h:250`/`229`).
- Produces (used by Task 9's orchestrator):
  ```cpp
  namespace ss::io::dawproject
  {
      void parseClips (const juce::XmlElement& root, Project& project, const ImportIds&,
                        ArchiveReader& archive, juce::StringArray& warningsOut);
  }
  ```

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("parseClips unwraps gain/warp wrappers and reads audio + MIDI clips")
        {
            juce::TemporaryFile wav (".wav");
            {
                juce::WavAudioFormat wavFormat;
                std::unique_ptr<juce::FileOutputStream> out (wav.getFile().createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wavFormat.createWriterFor (out.get(), 48000.0, 2, 16, {}, 0));
                out.release();
                juce::AudioBuffer<float> silence (2, 4800);
                silence.clear();
                writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
            }

            io::dawproject::ArchiveWriter archiveWriter;
            archiveWriter.addFile ("audio/take.wav", wav.getFile());
            juce::XmlElement dummyProjectXml ("Project");
            archiveWriter.addXml ("project.xml", dummyProjectXml);
            juce::TemporaryFile archiveFile (".dawproject");
            juce::String writeError;
            expect (archiveWriter.writeTo (archiveFile.getFile(), writeError), writeError);

            const auto xml = juce::String (R"XML(
<Project version="1.0">
  <Structure>
    <Track id="idGuitar" name="Guitar" contentType="audio"><Channel id="chGuitar" role="regular"/></Track>
    <Track id="idSynth"  name="Synth"  contentType="notes"><Channel id="chSynth"  role="regular"/></Track>
  </Structure>
  <Arrangement>
    <Lanes timeUnit="beats">
      <Clips track="idGuitar">
        <Clip time="4" duration="2">
          <Lanes>
            <Warps contentTimeUnit="seconds">
              <Audio channels="2" sampleRate="48000" duration="0.1"><File path="audio/take.wav"/></Audio>
              <Warp time="0" contentTime="0"/>
              <Warp time="2" contentTime="4"/>
            </Warps>
            <Points unit="decibel"><Target expression="gain"/><RealPoint time="0" value="-6"/></Points>
          </Lanes>
        </Clip>
      </Clips>
      <Clips track="idSynth">
        <Clip time="0" duration="4">
          <Notes>
            <Note time="0" duration="1" key="60" channel="0" vel="1.0"/>
          </Notes>
        </Clip>
      </Clips>
    </Lanes>
  </Arrangement>
</Project>
)XML");
            auto root = juce::parseXML (xml);
            expect (root != nullptr);

            Project project;
            juce::StringArray warnings;
            auto ids = io::dawproject::parseStructure (*root, project, warnings);

            io::dawproject::ArchiveReader reader (archiveFile.getFile());
            io::dawproject::parseClips (*root, project, ids, reader, warnings);

            auto& guitar = project.getTrack (0);
            expect (! guitar.audioClips.empty());
            auto& audioClip = guitar.audioClips[0];
            expectWithinAbsoluteError (audioClip.startBeats, 4.0, 1.0e-6);
            expectWithinAbsoluteError (audioClip.gainDb, -6.0f, 1.0e-4f);
            expectWithinAbsoluteError (audioClip.playbackRate, 2.0, 1.0e-6);   // (4-0)/(2-0)
            expect (audioClip.sourceFile.existsAsFile());

            auto& synth = project.getTrack (1);
            expect (! synth.midiClips.empty());
            expectEquals (synth.midiClips[0].notes[0].pitch, 60);
            expectEquals (synth.midiClips[0].notes[0].channel, 1);   // 0-based -> 1-based
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task7.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `parseClips` doesn't exist yet.

- [ ] **Step 3: Add the declaration to `Source/IO/DawProject.h`**

```cpp
    void parseClips (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                      ArchiveReader& archive, juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        struct UnwrappedClipContent
        {
            const juce::XmlElement* base = nullptr;   // <Audio> or <Notes>, never null if content was found
            float  gainDb = 0.0f;
            double playbackRate = 1.0;
        };

        UnwrappedClipContent unwrapClipContent (const juce::XmlElement& clipEl, const TempoMap& tempo)
        {
            UnwrappedClipContent result;
            const juce::XmlElement* current = clipEl.getFirstChildElement();
            const auto clipStartBeats = clipEl.getDoubleAttribute ("time");   // 0 for a session ClipSlot's Clip (Task 5/9)

            while (current != nullptr)
            {
                if (current->hasTagName ("Lanes"))
                {
                    const juce::XmlElement* inner = nullptr;
                    for (auto* c : current->getChildIterator())
                    {
                        if (c->hasTagName ("Points"))
                        {
                            auto* target = c->getChildByName ("Target");
                            if (target != nullptr && target->getStringAttribute ("expression") == "gain")
                                if (auto* pt = c->getChildByName ("RealPoint"))
                                    result.gainDb = (float) pt->getDoubleAttribute ("value");
                        }
                        else
                        {
                            inner = c;
                        }
                    }
                    current = inner;
                }
                else if (current->hasTagName ("Warps"))
                {
                    const juce::XmlElement* firstWarp = nullptr;
                    const juce::XmlElement* lastWarp = nullptr;
                    const juce::XmlElement* inner = nullptr;
                    for (auto* c : current->getChildIterator())
                    {
                        if (c->hasTagName ("Warp")) { if (firstWarp == nullptr) firstWarp = c; lastWarp = c; }
                        else inner = c;
                    }
                    if (firstWarp != nullptr && lastWarp != nullptr && firstWarp != lastWarp)
                    {
                        // Warp's "time" is beats (relative to the clip's own start), but
                        // "contentTime" is seconds (contentTimeUnit="seconds") - dividing
                        // them directly, with no tempo conversion, is a real bug found
                        // during Task 7's review: it silently returns the wrong playbackRate
                        // on any round trip through a non-60bpm project (verified by hand
                        // against Task 3's own export formula and its 120bpm test fixture).
                        const auto dTimeSeconds = tempo.beatsToSeconds (clipStartBeats + lastWarp->getDoubleAttribute ("time"))
                                                 - tempo.beatsToSeconds (clipStartBeats + firstWarp->getDoubleAttribute ("time"));
                        const auto dContent = lastWarp->getDoubleAttribute ("contentTime") - firstWarp->getDoubleAttribute ("contentTime");
                        if (dTimeSeconds > 1.0e-9)
                            result.playbackRate = dContent / dTimeSeconds;
                    }
                    current = inner;
                }
                else
                {
                    result.base = current;
                    break;
                }
            }
            return result;
        }

        void addAudioClipFromXml (Track& track, const juce::XmlElement& clipEl, const UnwrappedClipContent& unwrapped,
                                   Project& project, ArchiveReader& archive, juce::StringArray& warningsOut)
        {
            auto* fileEl = unwrapped.base->getChildByName ("File");
            if (fileEl == nullptr)
            {
                warningsOut.add ("Skipped an audio clip with no <File> reference.");
                return;
            }

            const auto storedPath = fileEl->getStringAttribute ("path");
            const auto targetFile = project.getMediaFolder().getChildFile (
                juce::File::createLegalFileName (storedPath.fromLastOccurrenceOf ("/", false, false)));

            if (! archive.extractEntryToFile (storedPath, targetFile))
            {
                warningsOut.add ("Skipped audio clip - could not extract \"" + storedPath + "\" from the archive.");
                return;
            }

            AudioClip clip;
            clip.id = project.nextClipId();
            clip.name = clipEl.getStringAttribute ("name");
            clip.sourceFile = targetFile;
            clip.startBeats = clipEl.getDoubleAttribute ("time");
            clip.lengthBeats = clipEl.getDoubleAttribute ("duration", 4.0);
            clip.fadeInSec = clipEl.getDoubleAttribute ("fadeInTime", 0.0);
            clip.fadeOutSec = clipEl.getDoubleAttribute ("fadeOutTime", 0.0);
            clip.gainDb = unwrapped.gainDb;
            clip.playbackRate = unwrapped.playbackRate;
            track.audioClips.push_back (clip);
        }

        void addMidiClipFromXml (Track& track, const juce::XmlElement& clipEl,
                                  const UnwrappedClipContent& unwrapped, Project& project)
        {
            MidiClip clip;
            clip.id = project.nextClipId();
            clip.name = clipEl.getStringAttribute ("name");
            clip.startBeats = clipEl.getDoubleAttribute ("time");
            clip.lengthBeats = clipEl.getDoubleAttribute ("duration", 4.0);

            for (auto* noteEl : unwrapped.base->getChildIterator())
            {
                if (! noteEl->hasTagName ("Note"))
                    continue;
                Note note;
                note.pitch       = noteEl->getIntAttribute ("key", 60);
                note.startBeats  = noteEl->getDoubleAttribute ("time");
                note.lengthBeats = noteEl->getDoubleAttribute ("duration", 1.0);
                note.channel     = noteEl->getIntAttribute ("channel", 0) + 1;   // 0-based -> 1-based
                note.velocity    = juce::jlimit (1, 127, (int) std::lround (noteEl->getDoubleAttribute ("vel", 100.0 / 127.0) * 127.0));
                clip.notes.push_back (note);
            }
            track.midiClips.push_back (clip);
        }
    }

    void parseClips (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                      ArchiveReader& archive, juce::StringArray& warningsOut)
    {
        auto* arrangement = root.getChildByName ("Arrangement");
        auto* lanes = arrangement != nullptr ? arrangement->getChildByName ("Lanes") : nullptr;
        if (lanes == nullptr)
            return;

        for (auto* clipsEl : lanes->getChildIterator())
        {
            if (! clipsEl->hasTagName ("Clips"))
                continue;

            const auto trackXml = clipsEl->getStringAttribute ("track");
            if (! ids.trackByXmlId.count (trackXml))
            {
                warningsOut.add ("Skipped clips for an unrecognised track (\"" + trackXml + "\").");
                continue;
            }
            auto* track = project.findTrack (ids.trackByXmlId.at (trackXml));
            if (track == nullptr)
                continue;

            for (auto* clipEl : clipsEl->getChildIterator())
            {
                if (! clipEl->hasTagName ("Clip"))
                    continue;

                const auto unwrapped = unwrapClipContent (*clipEl, project.tempo);
                if (unwrapped.base == nullptr)
                {
                    warningsOut.add ("Skipped a clip with no recognised content.");
                    continue;
                }

                if (unwrapped.base->hasTagName ("Audio"))
                    addAudioClipFromXml (*track, *clipEl, unwrapped, project, archive, warningsOut);
                else if (unwrapped.base->hasTagName ("Notes"))
                    addMidiClipFromXml (*track, *clipEl, unwrapped, project);
                else
                    warningsOut.add ("Skipped a clip of unsupported type <" + unwrapped.base->getTagName() + ">.");
            }
        }
    }
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task7.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task7.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task7.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Import DAWproject clips: audio (unwrapping gain/warp) and MIDI notes"
```

---

### Task 8: Import — Plugin state, builtin FX, and automation

Mirrors Task 4 in reverse, same per-track combined pass (devices must be
parsed before automation, since automation `Target/parameter` IDREFs
point at `id` attributes the device-parsing half assigns/reads on the way
in — `juce::XmlElement` has no parent pointer, so there is no
walk-up-the-tree shortcut; the resolution map is built top-down as
devices are visited, exactly as it was built top-down while being written
in Task 4).

A foreign file's automation may target far more plugin parameters than
KANADE DAW ever creates `id`s for (KANADE DAW only assigns one when *it*
automates something) — an unresolvable `Target/parameter` is expected and
common, not an error. Individual unresolved lanes are not each warned
about (that would be noisy on a real Bitwig/Studio One project); one
aggregate count is reported instead.

**Files:**
- Modify: `Source/IO/DawProject.h` (add `parseDevicesAndAutomation` declaration)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: `ImportIds`/`findTrackElement()` (Tasks 6/4), `ArchiveReader::
  readEntry()` (Task 1), `PluginManager::getKnownPluginList()` (existing).
- Produces (used by Task 9's orchestrator):
  ```cpp
  namespace ss::io::dawproject
  {
      void parseDevicesAndAutomation (const juce::XmlElement& root, Project& project, PluginManager& plugins,
                                       const ImportIds& ids, ArchiveReader& archive, juce::StringArray& warningsOut);
  }
  ```

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("parseDevicesAndAutomation rebuilds plugin state, builtin FX and automation")
        {
            const auto xml = juce::String (R"XML(
<Project version="1.0">
  <Structure>
    <Track id="idGuitar" name="Guitar" contentType="audio">
      <Channel id="chGuitar" role="regular">
        <Volume unit="decibel" value="0" id="volId"/>
        <Devices>
          <Vst3Plugin id="devPlugin" deviceRole="audioFX" deviceName="Nope" deviceVendor="Nobody" loaded="false">
            <State path="plugins/state.bin"/>
            <Parameters><RealParameter id="p0" parameterID="2" unit="normalized"/></Parameters>
          </Vst3Plugin>
          <BuiltinDevice id="devFx" deviceRole="audioFX">
            <Compressor>
              <Threshold unit="normalized" value="0.7" id="threshId"/>
              <Ratio unit="normalized" value="0.3"/>
              <OutputGain unit="normalized" value="0.5"/>
            </Compressor>
          </BuiltinDevice>
        </Devices>
      </Channel>
    </Track>
  </Structure>
  <Arrangement>
    <Lanes timeUnit="beats">
      <Points track="idGuitar" unit="normalized"><Target parameter="volId"/><RealPoint time="0" value="0.6"/></Points>
      <Points track="idGuitar" unit="normalized"><Target parameter="p0"/><RealPoint time="0" value="0.9"/></Points>
      <Points track="idGuitar" unit="normalized"><Target parameter="threshId"/><RealPoint time="0" value="0.4"/></Points>
    </Lanes>
  </Arrangement>
</Project>
)XML");
            auto root = juce::parseXML (xml);
            expect (root != nullptr);

            io::dawproject::ArchiveWriter archiveWriter;
            juce::MemoryBlock stateBytes ("plugin-state", 12);
            archiveWriter.addBytes ("plugins/state.bin", stateBytes);
            juce::XmlElement dummy ("Project");
            archiveWriter.addXml ("project.xml", dummy);
            juce::TemporaryFile archiveFile (".dawproject");
            juce::String writeError;
            expect (archiveWriter.writeTo (archiveFile.getFile(), writeError), writeError);

            Project project;
            juce::StringArray warnings;
            auto ids = io::dawproject::parseStructure (*root, project, warnings);

            Settings settings;
            PluginManager plugins (settings);
            io::dawproject::ArchiveReader reader (archiveFile.getFile());
            io::dawproject::parseDevicesAndAutomation (*root, project, plugins, ids, reader, warnings);

            auto& guitar = project.getTrack (0);
            expectEquals ((int) guitar.plugins.size(), 1);
            expect (guitar.plugins[0].state == stateBytes);

            expectEquals ((int) guitar.builtinFx.size(), 1);
            expectEquals (guitar.builtinFx[0].type, juce::String ("compressor"));
            expectWithinAbsoluteError ((double) guitar.builtinFx[0].params["threshold"], 0.7, 1.0e-6);
            expectWithinAbsoluteError ((double) guitar.builtinFx[0].params["makeup"], 0.5, 1.0e-6);

            bool foundGain = false, foundPlugin = false, foundFx = false;
            for (const auto& lane : guitar.automation)
            {
                if (lane.parameterId == "gain")            { foundGain = true;   expectWithinAbsoluteError (lane.points[0].second, 0.6f, 1.0e-4f); }
                if (lane.parameterId == "plugin:0:2")       { foundPlugin = true; expectWithinAbsoluteError (lane.points[0].second, 0.9f, 1.0e-4f); }
                if (lane.parameterId == "fx:0:threshold")   { foundFx = true;     expectWithinAbsoluteError (lane.points[0].second, 0.4f, 1.0e-4f); }
            }
            expect (foundGain && foundPlugin && foundFx);
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task8.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `parseDevicesAndAutomation` doesn't exist yet.

- [ ] **Step 3: Add the declaration to `Source/IO/DawProject.h`**

```cpp
    void parseDevicesAndAutomation (const juce::XmlElement& root, Project& project, PluginManager& plugins,
                                     const ImportIds& ids, ArchiveReader& archive, juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        using ParamIdMap = std::map<juce::String, juce::String>;   // xml id -> KANADE AutomationLane::parameterId

        void registerIfIdPresent (const juce::XmlElement* el, const juce::String& kanadeParamId, ParamIdMap& out)
        {
            if (el != nullptr && el->hasAttribute ("id"))
                out[el->getStringAttribute ("id")] = kanadeParamId;
        }

        void parseEqualizerParams (const juce::XmlElement& eq, ss::BuiltinFxSlot& fx, int slot, ParamIdMap& targetIds)
        {
            struct Band { int order; const char* freqId; const char* gainId; const char* qId; const char* onId; };
            static const Band bands[] = {
                { 0, "hpFreq",   nullptr,    nullptr,   "hpOn" },
                { 1, "lowFreq",  "lowGain",  nullptr,   nullptr },
                { 2, "mid1Freq", "mid1Gain", "mid1Q",   nullptr },
                { 3, "mid2Freq", "mid2Gain", "mid2Q",   nullptr },
                { 4, "highFreq", "highGain", nullptr,   nullptr },
                { 5, "lpFreq",   nullptr,    nullptr,   "lpOn" },
            };
            for (auto* bandEl : eq.getChildIterator())
            {
                if (! bandEl->hasTagName ("Band")) continue;
                const auto order = bandEl->getIntAttribute ("order", -1);
                for (const auto& band : bands)
                {
                    if (band.order != order) continue;
                    if (auto* freq = bandEl->getChildByName ("Freq"))
                    {
                        fx.params.set (band.freqId, freq->getDoubleAttribute ("value"));
                        registerIfIdPresent (freq, "fx:" + juce::String (slot) + ":" + band.freqId, targetIds);
                    }
                    if (band.gainId != nullptr)
                        if (auto* gain = bandEl->getChildByName ("Gain"))
                        {
                            fx.params.set (band.gainId, gain->getDoubleAttribute ("value"));
                            registerIfIdPresent (gain, "fx:" + juce::String (slot) + ":" + juce::String (band.gainId), targetIds);
                        }
                    if (band.qId != nullptr)
                        if (auto* q = bandEl->getChildByName ("Q"))
                        {
                            fx.params.set (band.qId, q->getDoubleAttribute ("value"));
                            registerIfIdPresent (q, "fx:" + juce::String (slot) + ":" + juce::String (band.qId), targetIds);
                        }
                    if (band.onId != nullptr)
                        if (auto* en = bandEl->getChildByName ("Enabled"))
                            fx.params.set (band.onId, en->getBoolAttribute ("value") ? 1.0 : 0.0);
                }
            }
        }

        // Reads `tag`'s value into fx.params[paramId], registering its id (if present) for automation.
        void parseFxParam (const juce::XmlElement& parent, const juce::String& tag, const juce::String& paramId,
                            ss::BuiltinFxSlot& fx, int slot, ParamIdMap& targetIds)
        {
            auto* el = parent.getChildByName (tag);
            if (el == nullptr) return;
            fx.params.set (paramId, el->getDoubleAttribute ("value"));
            registerIfIdPresent (el, "fx:" + juce::String (slot) + ":" + paramId, targetIds);
        }

        ss::BuiltinFxSlot parseBuiltinDevice (const juce::XmlElement& device, int slot, ParamIdMap& targetIds,
                                               juce::StringArray& warningsOut, const juce::String& trackName)
        {
            ss::BuiltinFxSlot fx;
            fx.bypassed = ! xmlBoolParam (device, "Enabled", true);

            if (auto* eq = device.getChildByName ("Equalizer"))
            {
                fx.type = "eq";
                parseEqualizerParams (*eq, fx, slot, targetIds);
            }
            else if (auto* comp = device.getChildByName ("Compressor"))
            {
                fx.type = "compressor";
                parseFxParam (*comp, "Threshold",  "threshold", fx, slot, targetIds);
                parseFxParam (*comp, "Ratio",      "ratio",     fx, slot, targetIds);
                parseFxParam (*comp, "Attack",     "attack",    fx, slot, targetIds);
                parseFxParam (*comp, "Release",    "release",   fx, slot, targetIds);
                parseFxParam (*comp, "OutputGain", "makeup",    fx, slot, targetIds);
            }
            else if (auto* gate = device.getChildByName ("NoiseGate"))
            {
                fx.type = "gate";
                parseFxParam (*gate, "Threshold", "threshold", fx, slot, targetIds);
                parseFxParam (*gate, "Range",     "range",     fx, slot, targetIds);
                parseFxParam (*gate, "Attack",    "attack",    fx, slot, targetIds);
                parseFxParam (*gate, "Release",   "release",   fx, slot, targetIds);
            }
            else if (auto* lim = device.getChildByName ("Limiter"))
            {
                fx.type = "limiter";
                parseFxParam (*lim, "InputGain", "input",   fx, slot, targetIds);
                parseFxParam (*lim, "Release",   "release", fx, slot, targetIds);
            }
            else
            {
                warningsOut.add ("Skipped an unrecognised built-in device on track \"" + trackName + "\".");
                fx.type = {};
            }
            return fx;
        }

        void parsePointsForTrack (const juce::XmlElement& root, Track& track, const juce::String& trackXmlIdStr,
                                   const ParamIdMap& targetIds, juce::StringArray& warningsOut)
        {
            auto* arrangement = root.getChildByName ("Arrangement");
            auto* lanes = arrangement != nullptr ? arrangement->getChildByName ("Lanes") : nullptr;
            if (lanes == nullptr) return;

            int unresolved = 0;
            for (auto* pointsEl : lanes->getChildIterator())
            {
                if (! pointsEl->hasTagName ("Points")) continue;
                if (pointsEl->getStringAttribute ("track") != trackXmlIdStr) continue;

                auto* target = pointsEl->getChildByName ("Target");
                if (target == nullptr) continue;
                const auto targetId = target->getStringAttribute ("parameter");
                if (! targetIds.count (targetId)) { ++unresolved; continue; }

                Track::AutomationLane lane;
                lane.parameterId = targetIds.at (targetId);
                for (auto* pt : pointsEl->getChildIterator())
                {
                    if (pt->hasTagName ("RealPoint"))
                        lane.points.emplace_back (pt->getDoubleAttribute ("time"), (float) pt->getDoubleAttribute ("value"));
                    else if (pt->hasTagName ("BoolPoint"))
                        lane.points.emplace_back (pt->getDoubleAttribute ("time"), pt->getBoolAttribute ("value") ? 1.0f : 0.0f);
                }
                if (! lane.points.empty())
                    track.automation.push_back (lane);
            }

            // The plan's own stated intent ("one aggregate count is reported instead" of
            // warning per-lane) was never actually wired up in the original draft - found
            // as a real gap by the Task 8 implementer's self-review, not a hypothetical.
            if (unresolved > 0)
                warningsOut.add (juce::String (unresolved) + " automation lane(s) on track \"" + track.name
                                  + "\" target a parameter KANADE DAW doesn't track and were skipped.");
        }
    }

    void parseDevicesAndAutomation (const juce::XmlElement& root, Project& project, PluginManager& plugins,
                                     const ImportIds& ids, ArchiveReader& archive, juce::StringArray& warningsOut)
    {
        auto* structure = root.getChildByName ("Structure");
        if (structure == nullptr) return;

        for (const auto& [xmlId, kanadeId] : ids.trackByXmlId)
        {
            auto* track = project.findTrack (kanadeId);
            auto* trackEl = findTrackElement (*structure, xmlId);
            if (track == nullptr || trackEl == nullptr) continue;
            auto* channel = trackEl->getChildByName ("Channel");
            if (channel == nullptr) continue;

            ParamIdMap targetIds;
            registerIfIdPresent (channel->getChildByName ("Volume"), "gain", targetIds);
            registerIfIdPresent (channel->getChildByName ("Pan"),    "pan",  targetIds);
            registerIfIdPresent (channel->getChildByName ("Mute"),   "mute", targetIds);

            if (auto* devices = channel->getChildByName ("Devices"))
            {
                int pluginSlot = 0, fxSlot = 0;
                for (auto* device : devices->getChildIterator())
                {
                    if (device->hasTagName ("Vst3Plugin"))
                    {
                        ss::PluginSlot slot;
                        slot.displayName  = device->getStringAttribute ("deviceName");
                        slot.isInstrument = device->getStringAttribute ("deviceRole") == "instrument";
                        slot.bypassed     = ! xmlBoolParam (*device, "Enabled", true);

                        const auto vendor = device->getStringAttribute ("deviceVendor");
                        const auto name   = device->getStringAttribute ("deviceName");
                        bool found = false;
                        for (const auto& desc : plugins.getKnownPluginList().getTypes())
                        {
                            if (desc.pluginFormatName == "VST3" && desc.manufacturerName == vendor && desc.name == name)
                            {
                                slot.identifier = desc.createIdentifierString();
                                found = true;
                                break;
                            }
                        }
                        if (! found)
                        {
                            slot.identifier = "missing:" + vendor + ":" + name;
                            warningsOut.add ("Plugin \"" + name + "\" by \"" + vendor + "\" on track \""
                                              + track->name + "\" is not installed - added as missing.");
                        }

                        if (auto* stateEl = device->getChildByName ("State"))
                        {
                            juce::MemoryBlock bytes;
                            if (archive.readEntry (stateEl->getStringAttribute ("path"), bytes))
                                slot.state = bytes;
                            else
                                // Found missing during Task 8's review: a corrupt/truncated
                                // archive or a missing zip entry left slot.state silently
                                // empty here, discarding the user's saved plugin state with
                                // zero warning - the in-file precedent for this exact
                                // situation (addAudioClipFromXml's extraction-failure path,
                                // Task 7) already warns; this one didn't. The slot itself is
                                // still kept - state loss shouldn't drop the slot, matching
                                // the missing-plugin philosophy of preserving what can be.
                                warningsOut.add ("Could not read saved state for plugin \"" + name
                                                  + "\" on track \"" + track->name + "\" - state was reset.");
                        }

                        track->plugins.push_back (slot);
                        const int thisSlot = pluginSlot++;

                        if (auto* params = device->getChildByName ("Parameters"))
                            for (auto* p : params->getChildIterator())
                                if (p->hasTagName ("RealParameter") && p->hasAttribute ("id"))
                                    registerIfIdPresent (p, "plugin:" + juce::String (thisSlot) + ":"
                                                             + juce::String (p->getIntAttribute ("parameterID")), targetIds);
                    }
                    else if (device->hasTagName ("BuiltinDevice"))
                    {
                        const int thisSlot = fxSlot++;
                        auto fx = parseBuiltinDevice (*device, thisSlot, targetIds, warningsOut, track->name);
                        if (fx.type.isNotEmpty())
                            track->builtinFx.push_back (fx);
                    }
                    else
                    {
                        // A foreign DAW's other plugin formats (ClapPlugin/AuPlugin/
                        // Vst2Plugin - KANADE DAW hosts VST3 only) or any other device tag
                        // this code doesn't know. Found missing during Task 8's review: the
                        // original draft had no fallback branch here at all, silently
                        // dropping these devices with zero warning - inconsistent with
                        // parseBuiltinDevice's own unrecognized-subtype case, which does warn.
                        warningsOut.add ("Skipped an unsupported device (<" + device->getTagName()
                                          + ">) on track \"" + track->name + "\" - KANADE DAW only hosts VST3 plugins.");
                    }
                }
            }

            parsePointsForTrack (root, *track, xmlId, targetIds, warningsOut);
        }
    }
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task8.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task8.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task8.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Import DAWproject devices (plugin state, builtin FX) and automation"
```

---

### Task 9: Import — Scenes and the `importDawProject` orchestrator

Mirrors Task 5. Also the task that establishes the "malformed archive
never partially mutates `project`" guarantee (Global Constraints) — see
that section above for why this is ordering-based rather than a
scratch-object swap (`Project` has no copy or move).

**Files:**
- Modify: `Source/IO/DawProject.h` (add `parseScenes` + the public
  `importDawProject` declaration)
- Modify: `Source/IO/DawProject.cpp` (implement)
- Modify: `Source/IO/DawProjectTests.cpp` (add this task's test)

**Interfaces:**
- Consumes: everything from Tasks 6-8, `unwrapClipContent()` (Task 7,
  reused as-is — a `ClipSlot`'s `Clip` can be gain/warp-wrapped exactly
  like an arrangement `Clip`).
- Produces (used by Task 10's UI wiring and full round-trip test):
  ```cpp
  namespace ss::io
  {
      bool importDawProject (const juce::File& source, Project&, PluginManager&,
                              juce::String& errorOut, juce::StringArray& warningsOut);
  }
  ```

- [ ] **Step 1: Write the failing test**

Append to `Source/IO/DawProjectTests.cpp`:

```cpp
        beginTest ("importDawProject reads structure/clips/scenes and rejects a corrupt file cleanly")
        {
            // Round-trip through our own exporter first, the cheapest way to get a
            // realistic, fully-formed archive to import back.
            Project original;
            original.name = "Round Trip";
            auto& scene = original.addScene ("Verse");
            auto& track = original.addTrack (TrackType::midi, "Synth");
            SessionClip sc;
            sc.kind = SessionClip::Kind::midi;
            sc.lengthBeats = 4.0;
            sc.notes.push_back ({ 64, 0.0, 1.0, 90, 1, 1.0f });
            track.setSessionClip (scene.id, sc);

            auto& audioTrack = original.addTrack (TrackType::audio, "Guitar");
            audioTrack.gainDb = -2.0f;

            juce::TemporaryFile temp (".dawproject");
            juce::String exportError;
            juce::StringArray exportWarnings;
            Settings settings;
            PluginManager plugins (settings);
            expect (io::exportDawProject (temp.getFile(), original, plugins, exportError, exportWarnings), exportError);

            Project imported;
            juce::String importError;
            juce::StringArray importWarnings;
            const bool ok = io::importDawProject (temp.getFile(), imported, plugins, importError, importWarnings);
            expect (ok, importError);

            expectEquals (imported.getNumTracks(), 2);
            bool foundSynth = false, foundGuitar = false;
            for (int i = 0; i < imported.getNumTracks(); ++i)
            {
                auto& t = imported.getTrack (i);
                if (t.name == "Synth")  { foundSynth = true;  expectEquals ((int) t.sessionSlots.size(), 1); }
                if (t.name == "Guitar") { foundGuitar = true; expectWithinAbsoluteError (t.gainDb, -2.0f, 1.0e-4f); }
            }
            expect (foundSynth && foundGuitar);

            // A genuinely malformed file must fail cleanly and leave `project` untouched.
            Project untouched;
            untouched.name = "Do Not Touch";
            juce::TemporaryFile badFile (".dawproject");
            badFile.getFile().replaceWithText ("this is not a zip file");

            juce::String badError;
            juce::StringArray badWarnings;
            const bool badOk = io::importDawProject (badFile.getFile(), untouched, plugins, badError, badWarnings);
            expect (! badOk);
            expect (badError.isNotEmpty());
            expectEquals (untouched.getNumTracks(), 0);
            expectEquals (untouched.name, juce::String ("Do Not Touch"));
        }
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --config Debug > build_task9.log 2>&1; echo "EXIT CODE: $?"
```

Expected: FAIL — `importDawProject`/`parseScenes` don't exist yet.

- [ ] **Step 3: Add the declarations**

In `Source/IO/DawProject.h`, inside `namespace ss::io::dawproject`:

```cpp
    void parseScenes (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                       ArchiveReader& archive, juce::StringArray& warningsOut);
```

In `namespace ss::io` (next to `exportDawProject` from Task 5):

```cpp
    bool importDawProject (const juce::File& source, Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut);
```

- [ ] **Step 4: Implement in `Source/IO/DawProject.cpp`**

```cpp
    namespace
    {
        void addSessionClipFromXml (Track& track, SceneId sceneId, const juce::XmlElement& clipEl,
                                     const UnwrappedClipContent& unwrapped, Project& project,
                                     ArchiveReader& archive, juce::StringArray& warningsOut)
        {
            SessionClip sc;
            sc.name = clipEl.getStringAttribute ("name");
            sc.lengthBeats = clipEl.getDoubleAttribute ("duration", 4.0);
            sc.gainDb = unwrapped.gainDb;
            sc.playbackRate = unwrapped.playbackRate;

            if (unwrapped.base->hasTagName ("Audio"))
            {
                sc.kind = SessionClip::Kind::audio;
                auto* fileEl = unwrapped.base->getChildByName ("File");
                if (fileEl == nullptr)
                {
                    warningsOut.add ("Skipped a session clip with no <File> reference.");
                    return;
                }
                const auto storedPath = fileEl->getStringAttribute ("path");
                const auto targetFile = project.getMediaFolder().getChildFile (
                    juce::File::createLegalFileName (storedPath.fromLastOccurrenceOf ("/", false, false)));
                if (! archive.extractEntryToFile (storedPath, targetFile))
                {
                    warningsOut.add ("Skipped session clip - could not extract \"" + storedPath + "\" from the archive.");
                    return;
                }
                sc.sourceFile = targetFile;
            }
            else if (unwrapped.base->hasTagName ("Notes"))
            {
                sc.kind = SessionClip::Kind::midi;
                for (auto* noteEl : unwrapped.base->getChildIterator())
                {
                    if (! noteEl->hasTagName ("Note"))
                        continue;
                    Note note;
                    note.pitch       = noteEl->getIntAttribute ("key", 60);
                    note.startBeats  = noteEl->getDoubleAttribute ("time");
                    note.lengthBeats = noteEl->getDoubleAttribute ("duration", 1.0);
                    note.channel     = noteEl->getIntAttribute ("channel", 0) + 1;
                    note.velocity    = juce::jlimit (1, 127, (int) std::lround (noteEl->getDoubleAttribute ("vel", 100.0 / 127.0) * 127.0));
                    sc.notes.push_back (note);
                }
            }
            else
            {
                warningsOut.add ("Skipped a session clip of unsupported type <" + unwrapped.base->getTagName() + ">.");
                return;
            }

            track.setSessionClip (sceneId, sc);
        }
    }

    void parseScenes (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                       ArchiveReader& archive, juce::StringArray& warningsOut)
    {
        auto* scenesEl = root.getChildByName ("Scenes");
        if (scenesEl == nullptr)
            return;

        for (auto* sceneEl : scenesEl->getChildIterator())
        {
            if (! sceneEl->hasTagName ("Scene"))
                continue;

            auto& scene = project.addScene (sceneEl->getStringAttribute ("name", "Scene"));
            auto* sceneLanes = sceneEl->getChildByName ("Lanes");
            if (sceneLanes == nullptr)
                continue;

            for (auto* slotEl : sceneLanes->getChildIterator())
            {
                if (! slotEl->hasTagName ("ClipSlot"))
                    continue;

                const auto trackXml = slotEl->getStringAttribute ("track");
                if (! ids.trackByXmlId.count (trackXml))
                    continue;
                auto* track = project.findTrack (ids.trackByXmlId.at (trackXml));
                if (track == nullptr)
                    continue;

                auto* clipEl = slotEl->getChildByName ("Clip");
                if (clipEl == nullptr)
                    continue;

                const auto unwrapped = unwrapClipContent (*clipEl, project.tempo);
                if (unwrapped.base == nullptr)
                    continue;

                addSessionClipFromXml (*track, scene.id, *clipEl, unwrapped, project, archive, warningsOut);
            }
        }
    }
}

namespace ss::io
{
    bool importDawProject (const juce::File& source, Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut)
    {
        dawproject::ArchiveReader archive (source);
        auto root = archive.readProjectXml (errorOut);
        if (root == nullptr)
            return false;   // nothing below has touched `project` yet

        auto ids = dawproject::parseStructure (*root, project, warningsOut);
        dawproject::parseClips (*root, project, ids, archive, warningsOut);
        dawproject::parseDevicesAndAutomation (*root, project, plugins, ids, archive, warningsOut);
        dawproject::parseScenes (*root, project, ids, archive, warningsOut);

        return true;
    }
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --config Debug > build_task9.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task9.log 2>&1; echo "EXIT CODE: $?"
grep "DawProject" test_task9.log
```

Expected: exit code 0, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add Source/IO/DawProject.h Source/IO/DawProject.cpp Source/IO/DawProjectTests.cpp
git commit -m "Wire up importDawProject: scenes, malformed-archive handling, end-to-end read"
```

---

### Task 10: UI wiring (File > Export/Import) and the full-fidelity round-trip test

Confirmed against `Source/UI/UiSupport.h:19-38`: `CommandIDs` is a plain
`enum` (not `enum class`) whose values back the user's saved keyboard
shortcuts — its own comment is explicit that **new ids must be appended
at the end**, never inserted next to their thematically-related neighbors
(`importAudio, importMidi, exportMidi, ...`), because inserting mid-enum
silently repoints every later shortcut. `exportDawProject`/
`importDawProject` go after the current last entry, `toggleAutomation`.

**Files:**
- Modify: `Source/UI/UiSupport.h` (append 2 command ids)
- Modify: `Source/UI/MainComponent.cpp` (handlers, menu items, command
  registration — same 4 places every existing File command touches:
  `getAllCommands`, `getCommandInfo`, `perform`, the menu-building code)
- Modify: `Source/IO/DawProjectTests.cpp` (add the full-fidelity test)

**Interfaces:**
- Consumes: `io::exportDawProject`/`io::importDawProject` (Tasks 5, 9),
  `performProjectEdit` (existing, `Source/UI/UiSupport.h:48`).
- Produces: nothing further — this is the plan's last task.

- [ ] **Step 1: Append the two command ids**

In `Source/UI/UiSupport.h`, change:

```cpp
            showPreferences, rescanPlugins,
            // New ids go on the END: the values are what the user's saved key
            // mappings refer to, so inserting one in the middle silently
            // repoints every shortcut after it.
            toggleAutomation
        };
```

to:

```cpp
            showPreferences, rescanPlugins,
            // New ids go on the END: the values are what the user's saved key
            // mappings refer to, so inserting one in the middle silently
            // repoints every shortcut after it.
            toggleAutomation,
            exportDawProject, importDawProject
        };
```

- [ ] **Step 2: Add the handler methods**

In `Source/UI/MainComponent.cpp`, inside the `Impl` class, right after
`exportMusicXml()` (the function ending at line 838 in the current file):

```cpp
        void showDawProjectWarnings (const juce::StringArray& warnings)
        {
            if (warnings.isEmpty())
                return;
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                    TRANS ("Some things could not be transferred"),
                                                    warnings.joinIntoString ("\n"), TRANS ("OK"), &owner);
        }

        void exportDawProject()
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (TRANS ("Export DAWproject"),
                                                            ctx.settings->getProjectsFolder(), "*.dawproject");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;
                file = file.withFileExtension ("dawproject");

                juce::String error;
                juce::StringArray warnings;
                if (! io::exportDawProject (file, *ctx.project, *ctx.plugins, error, warnings))
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), error, TRANS ("OK"), &owner);
                    return;
                }
                showDawProjectWarnings (warnings);
            });
        }

        void importDawProject()
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (TRANS ("Import DAWproject"),
                                                            juce::File(), "*.dawproject");
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                auto error = std::make_shared<juce::String>();
                auto warnings = std::make_shared<juce::StringArray>();
                bool ok = false;

                performProjectEdit (*ctx.project, TRANS ("Import DAWproject"), [this, file, error, warnings, &ok]
                {
                    ok = io::importDawProject (file, *ctx.project, *ctx.plugins, *error, *warnings);
                });

                if (! ok)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Import failed"), *error, TRANS ("OK"), &owner);
                else
                    showDawProjectWarnings (*warnings);
            });
        }
```

(`error`/`warnings` are captured as `shared_ptr` rather than by-reference
into the `performProjectEdit` lambda because that lambda's `edit`
parameter is a `std::function<void()>` that may outlive this scope in
principle — matching the existing codebase's `[this, files, audio, at]`
by-value-capture style in `importFiles()` rather than assuming the
closure runs synchronously.)

- [ ] **Step 3: Register in `getAllCommands`**

Change the array in `MainComponent.cpp` (currently ending
`... CommandIDs::exportMidi, CommandIDs::exportAudio,
CommandIDs::exportMusicXml,`) to also list the two new ids:

```cpp
            CommandIDs::importAudio, CommandIDs::importMidi, CommandIDs::importDawProject,
            CommandIDs::exportMidi, CommandIDs::exportAudio, CommandIDs::exportMusicXml, CommandIDs::exportDawProject,
```

- [ ] **Step 4: Register in `getCommandInfo`**

Add two new `case` blocks next to `exportMusicXml`'s (same file, same
switch):

```cpp
            case CommandIDs::importDawProject:
                info.setInfo (TRANS ("Import DAWproject..."), TRANS ("Import a .dawproject file"), TRANS ("File"), 0);
                info.setActive (hasProject);
                break;

            case CommandIDs::exportDawProject:
                info.setInfo (TRANS ("DAWproject..."), TRANS ("Export for Studio One, Bitwig, Cubase and other DAWs"), TRANS ("File"), 0);
                info.setActive (hasProject);
                break;
```

- [ ] **Step 5: Register in `perform`**

```cpp
            case CommandIDs::importDawProject: impl->importDawProject(); return true;
            case CommandIDs::exportDawProject: impl->exportDawProject(); return true;
```

- [ ] **Step 6: Add the menu items**

In the File-menu-building code (same block as the `exportMenu`/`menu.addCommandItem (&commands, CommandIDs::importMidi);` lines shown earlier):

```cpp
                menu.addCommandItem (&commands, CommandIDs::importAudio);
                menu.addCommandItem (&commands, CommandIDs::importMidi);
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

- [ ] **Step 7: Add English/Japanese strings**

Append to `Resources/lang/en.txt`:
```
Export DAWproject=Export DAWproject
Import DAWproject=Import DAWproject
Import DAWproject...=Import DAWproject...
Import a .dawproject file=Import a .dawproject file
DAWproject...=DAWproject...
Export for Studio One, Bitwig, Cubase and other DAWs=Export for Studio One, Bitwig, Cubase and other DAWs
Some things could not be transferred=Some things could not be transferred
```

Append to `Resources/lang/ja.txt`:
```
Export DAWproject=DAWproject を書き出し
Import DAWproject=DAWproject を読み込み
Import DAWproject...=DAWproject を読み込み...
Import a .dawproject file=.dawproject ファイルを読み込みます
DAWproject...=DAWproject...
Export for Studio One, Bitwig, Cubase and other DAWs=Studio One、Bitwig、Cubase など他のDAW向けに書き出します
Some things could not be transferred=一部の要素は転送できませんでした
```

(Match the existing two-column `=`-separated format already used in both
files — read a few surrounding lines in each before editing to confirm
exact formatting, since this step does not re-derive that convention.)

- [ ] **Step 8: Write the full-fidelity round-trip test**

Append to `Source/IO/DawProjectTests.cpp` — this is the integration proof
the spec's Testing Strategy calls for: every task's mapping exercised
together in one project, not just individually.

```cpp
        beginTest ("full round trip: structure, bus, clips, builtin FX, automation, scenes, markers")
        {
            Project original;
            original.name = "Full Fidelity";
            original.tempo.setEvents ({ { 0.0, 95.0 } });
            original.tempo.setTimeSignatures ({ { 0.0, 5, 4 } });
            original.markers.push_back ({ 16.0, "Bridge" });

            auto& bus = original.addBus ("Drum Bus");
            bus.gainDb = -1.5f;

            auto& drums = original.addTrack (TrackType::audio, "Drums");
            drums.outputBus = bus.id;
            drums.gainDb = 1.0f;

            ss::BuiltinFxSlot comp;
            comp.type = "compressor";
            comp.params.set ("threshold", 0.3);
            comp.params.set ("ratio", 0.5);
            comp.params.set ("attack", 0.2);
            comp.params.set ("release", 0.4);
            comp.params.set ("makeup", 0.6);
            drums.builtinFx.push_back (comp);
            drums.automation.push_back ({ "fx:0:threshold", { { 0.0, 0.3f }, { 8.0, 0.1f } } });
            drums.automation.push_back ({ "gain", { { 0.0, 0.5f }, { 8.0, 0.9f } } });

            juce::TemporaryFile wav (".wav");
            {
                juce::WavAudioFormat wavFormat;
                std::unique_ptr<juce::FileOutputStream> out (wav.getFile().createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wavFormat.createWriterFor (out.get(), 48000.0, 2, 16, {}, 0));
                out.release();
                juce::AudioBuffer<float> silence (2, 4800);
                silence.clear();
                writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
            }
            auto& kickClip = drums.audioClips.emplace_back();
            kickClip.id = original.nextClipId();
            kickClip.sourceFile = wav.getFile();
            kickClip.startBeats = 0.0;
            kickClip.lengthBeats = 4.0;

            auto& synth = original.addTrack (TrackType::midi, "Synth");
            auto& synthClip = synth.midiClips.emplace_back();
            synthClip.id = original.nextClipId();
            synthClip.startBeats = 0.0;
            synthClip.lengthBeats = 4.0;
            synthClip.notes.push_back ({ 72, 0.0, 2.0, 110, 1, 1.0f });

            auto& scene = original.addScene ("Chorus");
            SessionClip sessionClip;
            sessionClip.kind = SessionClip::Kind::midi;
            sessionClip.lengthBeats = 2.0;
            sessionClip.notes.push_back ({ 75, 0.0, 1.0, 100, 1, 1.0f });
            synth.setSessionClip (scene.id, sessionClip);

            juce::TemporaryFile archive (".dawproject");
            juce::String error;
            juce::StringArray exportWarnings;
            Settings settings;
            PluginManager plugins (settings);
            expect (io::exportDawProject (archive.getFile(), original, plugins, error, exportWarnings), error);

            Project imported;
            juce::StringArray importWarnings;
            expect (io::importDawProject (archive.getFile(), imported, plugins, error, importWarnings), error);

            expectEquals (imported.getNumTracks(), 2);
            expectEquals ((int) imported.buses.size(), 1);
            expectWithinAbsoluteError (imported.tempo.bpmAt (0.0), 95.0, 1.0e-6);
            const auto ts = imported.tempo.timeSignatureAt (0.0);
            expectEquals (ts.numerator, 5);
            expectEquals (ts.denominator, 4);
            expect (! imported.markers.empty());
            expectWithinAbsoluteError (imported.markers[0].beat, 16.0, 1.0e-6);

            Track* importedDrums = nullptr;
            Track* importedSynth = nullptr;
            for (int i = 0; i < imported.getNumTracks(); ++i)
            {
                auto& t = imported.getTrack (i);
                if (t.name == "Drums") importedDrums = &t;
                if (t.name == "Synth") importedSynth = &t;
            }
            expect (importedDrums != nullptr && importedSynth != nullptr);

            expectEquals (importedDrums->outputBus, imported.buses[0].id);
            expectWithinAbsoluteError (imported.buses[0].gainDb, -1.5f, 1.0e-3f);
            expect (! importedDrums->audioClips.empty());
            expect (importedDrums->audioClips[0].sourceFile.existsAsFile());
            expect (! importedDrums->builtinFx.empty());
            expectEquals (importedDrums->builtinFx[0].type, juce::String ("compressor"));
            expectWithinAbsoluteError ((double) importedDrums->builtinFx[0].params["threshold"], 0.3, 1.0e-6);

            bool foundFxAutomation = false, foundGainAutomation = false;
            for (const auto& lane : importedDrums->automation)
            {
                if (lane.parameterId == "fx:0:threshold") foundFxAutomation = true;
                if (lane.parameterId == "gain")            foundGainAutomation = true;
            }
            expect (foundFxAutomation && foundGainAutomation);

            expect (! importedSynth->midiClips.empty());
            expectEquals (importedSynth->midiClips[0].notes[0].pitch, 72);
            expectEquals ((int) importedSynth->sessionSlots.size(), 1);
        }
```

- [ ] **Step 9: Run the full test suite**

```bash
cmake --build build --config Debug > build_task10.log 2>&1; echo "EXIT CODE: $?"
./build/*/KANADE\ DAW.exe --run-tests > test_task10.log 2>&1; echo "EXIT CODE: $?"
grep -E "DawProject|TOTAL" test_task10.log
```

Expected: exit code 0 for the build; the test log shows every
`DawProject` test passing and the overall `TOTAL` line shows 0 failures
(the full suite, not just this feature's tests — a UI-adjacent change
like a new menu command is exactly the kind of thing that can break an
unrelated existing test if a command id or menu index assumption
elsewhere in the codebase turns out to depend on the previous enum's
exact layout).

- [ ] **Step 10: Manually verify the menu items in a running build**

This plan's automated tests cover the mapping logic; they cannot click a
menu. Launch `KANADE DAW.exe`, open or create a project, and confirm:
`File > Export > DAWproject...` produces a `.dawproject` file, and
`File > Import DAWproject...` on that same file round-trips visibly
(tracks/clips reappear). This is the same category of manual check this
session already did for the VST3 crash fix (TH3/Melda) — automated tests
prove the mapping is correct, a human confirms the file actually opens in
a real DAW.

- [ ] **Step 11: Commit**

```bash
git add Source/UI/UiSupport.h Source/UI/MainComponent.cpp Resources/lang/en.txt Resources/lang/ja.txt \
        Source/IO/DawProjectTests.cpp
git commit -m "Wire up DAWproject Export/Import menu commands; add full-fidelity round-trip test"
```

---

## Self-Review

**Spec coverage:** Goal (export+import, Studio One/Bitwig/Cubase target) →
Tasks 5, 9, 10. Tech Stack (JUCE zip/xml, no new dep) → Task 1. Scope
in/out (utau, reverb/delay, bus/master FX, complex warp collapse) →
warned in Tasks 2/4/6/8 respectively. Data Model table → every row has a
task: tracks/buses/master/tempo/markers (Tasks 2/6), clips (3/7),
automation/plugin-state/builtin-FX (4/8), scenes (5/9). Export/Import
algorithm sections → Tasks 5/9 orchestrators. Error Handling
(`errorOut` vs `warningsOut`, never conflated) → enforced throughout,
tested explicitly in Task 9. UI → Task 10. Testing Strategy's four
bullet points (round-trip every field, dummy plugin state round-trip,
corrupt-file rejection, warning-producing elements) → covered by Tasks
1/8 (dummy state), 9 (corrupt file + structural round trip), 10 (full
fidelity + warnings implicitly exercised across every task's tests).

**Placeholder scan:** No TBD/TODO. Two explicitly-flagged
not-yet-schema-verified conventions (MIDI `vel`/`channel` in Task 3) are
concrete decisions with a stated fallback if wrong, not placeholders. The
bus/master effect chain gap (found during planning, not before) is
scoped out explicitly in both the spec and Task 4, with a `warningsOut`
entry rather than a silent drop — an honest scope cut, not a placeholder.

**Type consistency:** `ArchiveWriter`/`ArchiveReader` (Task 1) used
identically in Tasks 3, 4, 5, 7, 8, 9. `trackXmlId`/`channelXmlId`/
`busTrackXmlId`/`busChannelXmlId`/`masterTrackXmlId`/`masterChannelXmlId`
(Task 2) consumed as declared by Tasks 3-5. `ImportIds` (Task 6) consumed
identically by Tasks 7-9. `warningsOut : juce::StringArray&` and
`errorOut : juce::String&` keep the same meaning and position in every
function signature across all ten tasks. `unwrapClipContent`/
`UnwrappedClipContent` (Task 7) reused as-is by Task 9 for `ClipSlot`
content, per its own Interfaces note.

## Execution Handoff

Plan complete and saved to
`docs/superpowers/plans/2026-08-28-dawproject-interop.md`. Two execution
options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per
task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using
executing-plans, batch execution with checkpoints

**Which approach?**
