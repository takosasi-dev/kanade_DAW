#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

/*  Wire format shared by the plugin-host WORKER (PluginHostWorker.cpp) and the
    in-process PROXY that stands in for it (PluginHostProxy.h/.cpp).  Gap 16:
    every plugin instance runs in its own child process so a crash there
    cannot take the DAW with it (spec 10.3 / 15.6).

    Both ends talk over a single juce::ChildProcessCoordinator /
    juce::ChildProcessWorker pipe - the same mechanism PluginManager.cpp
    already uses for out-of-process scanning.  One pipe carries BOTH the
    realtime `process` round trip and the non-realtime control calls
    (load/state/editor); there is no separate low-latency channel.

    ponytail: a slow control call (e.g. openEditor while the format loads a
    big preset) shares the one pipe with `process`, so it can add up to its
    own timeout of glitch to that instance for one block. Upgrade path: give
    the audio path its own shared-memory ring + event if this is audible.

    Every command below is request (parent -> worker) / response (worker ->
    parent) unless marked PUSH, which is unsolicited worker -> parent and has
    no reply. Only one request is ever in flight per proxy - see
    PluginHostProxy's single "call" mutex - so there is no need for the
    messages themselves to carry a request id.

    All helpers here are `inline` and shared verbatim by both ends: this is
    the actual protocol, not a description of one - do not hand-roll the byte
    layout in either PluginHostWorker.cpp or PluginHostProxy.cpp.          */

namespace ss::hostproto
{
    enum class Cmd : juce::uint8
    {
        load = 1,          // load a plugin, get back its shape (channels, params, ...)
        prepare,           // sampleRate/blockSize changed
        release,           // releaseResources()
        process,           // one audio block, batched parameter changes ride along
        getState,          // AudioProcessor::getStateInformation()
        setState,          // AudioProcessor::setStateInformation()
        openEditor,        // create the plugin's editor, embedded as a child window
        resizeEditor,      // host-side container resized; worker resizes the editor to match
        closeEditor,       // destroy the editor (plugin instance keeps running)
        heartbeat,         // liveness probe for the watchdog - answered without touching
                           // the message thread, so a slow editor can't fake a hang
        editorResized,     // PUSH: the plugin resized its own editor, worker -> parent
    };

    inline void writeCmd (juce::MemoryOutputStream& out, Cmd cmd) { out.writeByte ((char) cmd); }
    inline Cmd  readCmd  (juce::MemoryInputStream& in)            { return (Cmd) in.readByte(); }

    //==========================================================================
    // load
    //   req:  descriptionXml (juce::PluginDescription::createXml(), as text),
    //         sampleRate (double), blockSize (int), initialState (block)
    //   resp: ok (bool)
    //         if ok:  name (string), acceptsMidi (bool), producesMidi (bool),
    //                 isInstrument (bool), numInputChannels (int),
    //                 numOutputChannels (int), latencySamples (int),
    //                 tailLengthSeconds (double), numParams (int), then per
    //                 param: name (string), label (string), isDiscrete (bool),
    //                 numSteps (int), defaultValue (float, 0..1)
    //         if !ok: errorMessage (string)
    struct ParamShape
    {
        juce::String name, label;
        bool isDiscrete = false;
        int numSteps = 0;
        float defaultValue = 0.0f;
    };

    struct LoadResult
    {
        bool ok = false;
        juce::String errorMessage;
        juce::String name;
        bool acceptsMidi = false, producesMidi = false, isInstrument = false;
        int numInputChannels = 0, numOutputChannels = 0, latencySamples = 0;
        double tailLengthSeconds = 0.0;
        std::vector<ParamShape> params;
    };

    inline void writeLoadRequest (juce::MemoryOutputStream& out, const juce::String& descriptionXml,
                                  double sampleRate, int blockSize, const juce::MemoryBlock& initialState)
    {
        writeCmd (out, Cmd::load);
        out.writeString (descriptionXml);
        out.writeDouble (sampleRate);
        out.writeInt (blockSize);
        out.writeCompressedInt ((int) initialState.getSize());
        out.write (initialState.getData(), initialState.getSize());
    }

    /** Reads everything after the Cmd byte. `xmlOut` receives the description text. */
    inline void readLoadRequest (juce::MemoryInputStream& in, juce::String& xmlOut, double& sampleRateOut,
                                 int& blockSizeOut, juce::MemoryBlock& stateOut)
    {
        xmlOut = in.readString();
        sampleRateOut = in.readDouble();
        blockSizeOut = in.readInt();
        const auto stateSize = in.readCompressedInt();
        stateOut.setSize ((size_t) juce::jmax (0, stateSize));
        if (stateSize > 0)
            in.read (stateOut.getData(), stateSize);
    }

    inline void writeLoadResponse (juce::MemoryOutputStream& out, const LoadResult& r)
    {
        out.writeBool (r.ok);

        if (! r.ok)
        {
            out.writeString (r.errorMessage);
            return;
        }

        out.writeString (r.name);
        out.writeBool (r.acceptsMidi);
        out.writeBool (r.producesMidi);
        out.writeBool (r.isInstrument);
        out.writeInt (r.numInputChannels);
        out.writeInt (r.numOutputChannels);
        out.writeInt (r.latencySamples);
        out.writeDouble (r.tailLengthSeconds);
        out.writeInt ((int) r.params.size());

        for (const auto& p : r.params)
        {
            out.writeString (p.name);
            out.writeString (p.label);
            out.writeBool (p.isDiscrete);
            out.writeInt (p.numSteps);
            out.writeFloat (p.defaultValue);
        }
    }

    inline LoadResult readLoadResponse (juce::MemoryInputStream& in)
    {
        LoadResult r;
        r.ok = in.readBool();

        if (! r.ok)
        {
            r.errorMessage = in.readString();
            return r;
        }

        r.name = in.readString();
        r.acceptsMidi = in.readBool();
        r.producesMidi = in.readBool();
        r.isInstrument = in.readBool();
        r.numInputChannels = in.readInt();
        r.numOutputChannels = in.readInt();
        r.latencySamples = in.readInt();
        r.tailLengthSeconds = in.readDouble();
        const auto numParams = in.readInt();

        r.params.resize ((size_t) juce::jmax (0, numParams));

        for (auto& p : r.params)
        {
            p.name = in.readString();
            p.label = in.readString();
            p.isDiscrete = in.readBool();
            p.numSteps = in.readInt();
            p.defaultValue = in.readFloat();
        }

        return r;
    }

    //==========================================================================
    // prepare
    //   req:  sampleRate (double), blockSize (int)
    //   resp: ok (bool)
    inline void writePrepareRequest (juce::MemoryOutputStream& out, double sampleRate, int blockSize)
    {
        writeCmd (out, Cmd::prepare);
        out.writeDouble (sampleRate);
        out.writeInt (blockSize);
    }

    inline void readPrepareRequest (juce::MemoryInputStream& in, double& sampleRateOut, int& blockSizeOut)
    {
        sampleRateOut = in.readDouble();
        blockSizeOut = in.readInt();
    }

    //==========================================================================
    // release  (req has no payload; resp is a bare ok bool)
    inline void writeReleaseRequest (juce::MemoryOutputStream& out) { writeCmd (out, Cmd::release); }

    //==========================================================================
    // Shared bool-only ack, used by prepare / release / setState / resizeEditor / closeEditor / heartbeat.
    inline void writeAck (juce::MemoryOutputStream& out, bool ok) { out.writeBool (ok); }
    inline bool readAck  (juce::MemoryInputStream& in)            { return in.readBool(); }

    //==========================================================================
    // Raw audio + MIDI blocks, shared by the `process` request and response.
    //   numChannels (int), numSamples (int), then each channel's samples as a
    //   raw native-endian float dump (both ends are the same machine/build -
    //   no byte-swapping, no per-sample calls).
    //   MIDI: numEvents (int), then per event samplePosition (int),
    //   numBytes (int), raw bytes.
    inline void writeAudioBlock (juce::MemoryOutputStream& out, const juce::AudioBuffer<float>& buffer)
    {
        const auto nc = buffer.getNumChannels();
        const auto n  = buffer.getNumSamples();
        out.writeInt (nc);
        out.writeInt (n);

        for (int ch = 0; ch < nc; ++ch)
            out.write (buffer.getReadPointer (ch), (size_t) n * sizeof (float));
    }

    /** Resizes `buffer` to fit and reads into it. */
    inline void readAudioBlock (juce::MemoryInputStream& in, juce::AudioBuffer<float>& buffer)
    {
        const auto nc = in.readInt();
        const auto n  = in.readInt();
        buffer.setSize (juce::jmax (0, nc), juce::jmax (0, n), false, false, true);

        for (int ch = 0; ch < nc; ++ch)
            in.read (buffer.getWritePointer (ch), (size_t) n * sizeof (float));
    }

    inline void writeMidiBlock (juce::MemoryOutputStream& out, const juce::MidiBuffer& midi)
    {
        out.writeInt (midi.getNumEvents());

        for (const auto metadata : midi)
        {
            out.writeInt (metadata.samplePosition);
            out.writeInt (metadata.numBytes);
            out.write (metadata.data, (size_t) metadata.numBytes);
        }
    }

    inline void readMidiBlock (juce::MemoryInputStream& in, juce::MidiBuffer& midi)
    {
        midi.clear();
        const auto numEvents = in.readInt();

        for (int i = 0; i < numEvents; ++i)
        {
            const auto pos = in.readInt();
            const auto numBytes = in.readInt();
            juce::HeapBlock<juce::uint8> bytes ((size_t) numBytes);
            in.read (bytes, numBytes);
            midi.addEvent (bytes, numBytes, pos);
        }
    }

    //==========================================================================
    // process
    //   req:  numChangedParams (int), then (paramIndex int, value float) pairs -
    //         setValue() calls made on the audio thread just record locally and
    //         ride along on the next block instead of round-tripping alone -,
    //         then audio-in block, then midi-in block
    //   resp: audio-out block, then midi-out block
    struct ParamChange { int index; float value; };

    inline void writeProcessRequest (juce::MemoryOutputStream& out, const std::vector<ParamChange>& changes,
                                     const juce::AudioBuffer<float>& audioIn, const juce::MidiBuffer& midiIn)
    {
        writeCmd (out, Cmd::process);
        out.writeInt ((int) changes.size());

        for (const auto& c : changes)
        {
            out.writeInt (c.index);
            out.writeFloat (c.value);
        }

        writeAudioBlock (out, audioIn);
        writeMidiBlock (out, midiIn);
    }

    inline void readProcessRequest (juce::MemoryInputStream& in, std::vector<ParamChange>& changesOut,
                                    juce::AudioBuffer<float>& audioInOut, juce::MidiBuffer& midiInOut)
    {
        const auto numChanges = in.readInt();
        changesOut.resize ((size_t) juce::jmax (0, numChanges));

        for (auto& c : changesOut)
        {
            c.index = in.readInt();
            c.value = in.readFloat();
        }

        readAudioBlock (in, audioInOut);
        readMidiBlock (in, midiInOut);
    }

    inline void writeProcessResponse (juce::MemoryOutputStream& out, const juce::AudioBuffer<float>& audioOut,
                                      const juce::MidiBuffer& midiOut)
    {
        writeAudioBlock (out, audioOut);
        writeMidiBlock (out, midiOut);
    }

    inline void readProcessResponse (juce::MemoryInputStream& in, juce::AudioBuffer<float>& audioOutOut,
                                     juce::MidiBuffer& midiOutOut)
    {
        readAudioBlock (in, audioOutOut);
        readMidiBlock (in, midiOutOut);
    }

    //==========================================================================
    // getState / setState
    //   getState req: none.  resp: block (compressed-int size, then raw bytes)
    //   setState req: block (same shape).  resp: ack bool
    inline void writeStateBlock (juce::MemoryOutputStream& out, const juce::MemoryBlock& state)
    {
        out.writeCompressedInt ((int) state.getSize());
        out.write (state.getData(), state.getSize());
    }

    inline void readStateBlock (juce::MemoryInputStream& in, juce::MemoryBlock& stateOut)
    {
        const auto size = in.readCompressedInt();
        stateOut.setSize ((size_t) juce::jmax (0, size));
        if (size > 0)
            in.read (stateOut.getData(), size);
    }

    //==========================================================================
    // openEditor
    //   req:  parentNativeHandle (int64) - the host placeholder window the
    //         worker should reparent its editor window into (Windows: an HWND
    //         value, valid because both processes share the same desktop
    //         session; see PluginHostProxy's editor component for the other
    //         side of the SetParent() call)
    //   resp: ok (bool); if ok: width (int), height (int), resizable (bool);
    //         if !ok: "no editor" is not an error, just ok=false with an empty
    //         errorMessage - the proxy shows its own placeholder for that
    //   ponytail (Windows-only reparenting): on Mac the worker instead opens
    //   the editor in its own top-level window rather than embedding it -
    //   there is no development or test rig for the NSView-remoting this would
    //   otherwise need. See PluginHostWorker.cpp.
    inline void writeOpenEditorRequest (juce::MemoryOutputStream& out, juce::int64 parentNativeHandle)
    {
        writeCmd (out, Cmd::openEditor);
        out.writeInt64 (parentNativeHandle);
    }

    inline juce::int64 readOpenEditorRequest (juce::MemoryInputStream& in) { return in.readInt64(); }

    struct OpenEditorResult { bool ok = false; int width = 0, height = 0; bool resizable = false; };

    inline void writeOpenEditorResponse (juce::MemoryOutputStream& out, const OpenEditorResult& r)
    {
        out.writeBool (r.ok);
        out.writeInt (r.width);
        out.writeInt (r.height);
        out.writeBool (r.resizable);
    }

    inline OpenEditorResult readOpenEditorResponse (juce::MemoryInputStream& in)
    {
        OpenEditorResult r;
        r.ok = in.readBool();
        r.width = in.readInt();
        r.height = in.readInt();
        r.resizable = in.readBool();
        return r;
    }

    //==========================================================================
    // resizeEditor:  req width(int), height(int).  resp: ack bool.
    inline void writeResizeEditorRequest (juce::MemoryOutputStream& out, int width, int height)
    {
        writeCmd (out, Cmd::resizeEditor);
        out.writeInt (width);
        out.writeInt (height);
    }

    inline void readResizeEditorRequest (juce::MemoryInputStream& in, int& widthOut, int& heightOut)
    {
        widthOut = in.readInt();
        heightOut = in.readInt();
    }

    //==========================================================================
    // closeEditor: no payload either way besides the Cmd byte and an ack.

    //==========================================================================
    // editorResized (PUSH, worker -> parent): width(int), height(int).
    inline void writeEditorResizedPush (juce::MemoryOutputStream& out, int width, int height)
    {
        writeCmd (out, Cmd::editorResized);
        out.writeInt (width);
        out.writeInt (height);
    }

    inline void readEditorResizedPush (juce::MemoryInputStream& in, int& widthOut, int& heightOut)
    {
        widthOut = in.readInt();
        heightOut = in.readInt();
    }

    //==========================================================================
    /** The command-line marker that tells Main.cpp's relaunch of this same
        executable "you are a plugin-host worker", mirroring `scannerUID` in
        PluginManager.cpp. Kept here so both PluginHostWorker.cpp (reads it via
        runPluginHostProcessIfRequested) and PluginHostProxy.cpp (passes it to
        launchWorkerProcess) use the exact same string. */
    inline const char* workerUID() { return "scoresmith-plugin-host"; }
}
