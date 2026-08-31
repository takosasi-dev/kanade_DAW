#include "Mixer/Mixer.h"
#include "Plugins/PluginManager.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>

namespace ss
{

namespace
{
    constexpr int maxMeterChannels = 8;
    constexpr int stripChannels    = 2;   // every strip is stereo; the pan law needs it

    void addAllNotesOff (juce::MidiBuffer& dest)
    {
        for (int ch = 1; ch <= 16; ++ch)
            dest.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
    }
}

void equalPowerPanGains (float pan, float& leftGain, float& rightGain) noexcept
{
    const auto angle = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * 0.25f
                         * juce::MathConstants<float>::pi;
    /*  cos(pi/2) comes back as about -4.4e-8 in single precision, so the hard
        left/right ends would otherwise invert the (silent) opposite channel.   */
    leftGain  = juce::jmax (0.0f, std::cos (angle));
    rightGain = juce::jmax (0.0f, std::sin (angle));
}

float automationValueAt (const std::vector<std::pair<double, float>>& points,
                         double beat, float fallback) noexcept
{
    if (points.empty())
        return fallback;

    // Held, not extrapolated: a lane must never invent a value outside the range
    // the user actually drew.
    if (beat <= points.front().first)  return points.front().second;
    if (beat >= points.back().first)   return points.back().second;

    /*  ponytail: linear scan, O(points) per block per lane.  Lanes are drawn by
        hand and playback is almost always forward, so caching the last index (or
        a binary search) only pays off on a lane with thousands of points.       */
    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto& b = points[i];

        if (beat <= b.first)
        {
            const auto& a = points[i - 1];
            const auto span = b.first - a.first;

            // Two points on the same beat: the later one wins, so a lane can step.
            if (span <= 0.0)
                return b.second;

            return a.second + (b.second - a.second) * (float) ((beat - a.first) / span);
        }
    }

    return points.back().second;
}

//==============================================================================
struct ChannelStrip::Impl
{
    explicit Impl (PluginManager& pm) : plugins (pm)
    {
        for (auto& v : peak) v.store (0.0f);
        for (auto& v : rms)  v.store (0.0f);
    }

    PluginManager& plugins;

    double sampleRate = 44100.0;
    int blockSize   = 512;
    int numChannels = stripChannels;

    std::unique_ptr<juce::AudioPluginInstance> instrument;
    juce::String instrumentIdentifier;      // the slot.identifier `instrument` was created from
    std::vector<std::unique_ptr<BuiltinEffect>> builtinFx;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> pluginFx;
    std::vector<juce::String> pluginIdentifier;  // pluginFx index -> the slot.identifier it was created from
    /*  Read on the audio thread and flipped live from the message thread, so the
        two ends meet through std::atomic_ref rather than a plain byte write. */
    std::vector<char> pluginBypassed;
    std::vector<int>  pluginSlotIndex;      // pluginFx index -> the track's own slot index
    int instrumentSlotIndex = -1;

    juce::AudioBuffer<float> instrumentScratch, pluginScratch;

    std::atomic<float> gainDb { 0.0f }, pan { 0.0f };
    std::atomic<bool>  muted { false };
    bool balanceLaw = false;        // set for bus strips; see process()
    std::array<std::atomic<float>, maxMeterChannels> peak, rms;

    float lastLeft = 1.0f, lastRight = 1.0f, lastGain = 1.0f;

    void prepareInstance (juce::AudioPluginInstance& inst)
    {
        inst.setRateAndBufferSizeDetails (sampleRate, blockSize);
        inst.prepareToPlay (sampleRate, blockSize);
    }

    void resizeScratch()
    {
        auto instrumentChannels = numChannels;
        if (instrument != nullptr)
            instrumentChannels = juce::jmax (instrumentChannels,
                                             instrument->getTotalNumInputChannels(),
                                             instrument->getTotalNumOutputChannels());

        auto fxChannels = numChannels;
        for (auto& fx : pluginFx)
            fxChannels = juce::jmax (fxChannels, fx->getTotalNumInputChannels(),
                                     fx->getTotalNumOutputChannels());

        instrumentScratch.setSize (juce::jmax (1, instrumentChannels), blockSize, false, true, false);
        pluginScratch.setSize (juce::jmax (1, fxChannels), blockSize, false, true, false);
    }

    /** Runs a plugin whose bus layout is wider OR narrower than the strip
        through a pre-allocated scratch buffer, so the audio thread never
        resizes either `buffer` or the plugin's own channel count. */
    void runPlugin (juce::AudioPluginInstance& inst, juce::AudioBuffer<float>& buffer,
                    juce::MidiBuffer& midi)
    {
        const auto n = buffer.getNumSamples();
        const auto bufferChannels = buffer.getNumChannels();
        const auto needed = juce::jmax (inst.getTotalNumInputChannels(),
                                        inst.getTotalNumOutputChannels());

        if (needed == bufferChannels)
        {
            inst.processBlock (buffer, midi);
            return;
        }

        if (needed <= 0 || needed > pluginScratch.getNumChannels() || n > pluginScratch.getNumSamples())
            return;

        juce::AudioBuffer<float> view (pluginScratch.getArrayOfWritePointers(), needed, n);
        view.clear();

        if (needed > bufferChannels)
        {
            for (int ch = 0; ch < bufferChannels; ++ch)
                view.copyFrom (ch, 0, buffer, ch, 0, n);

            inst.processBlock (view, midi);

            for (int ch = 0; ch < bufferChannels; ++ch)
                buffer.copyFrom (ch, 0, view, ch, 0, n);

            return;
        }

        // needed < bufferChannels: a mono (or otherwise narrower-than-the-
        // strip) plugin. Calling processBlock() directly on the wider
        // `buffer` used to leave every channel beyond the plugin's own bus
        // width untouched by it - silent for a freshly-read/IPC'd buffer,
        // since nothing else in the chain wrote to it either. Average every
        // buffer channel down into the plugin's narrower width instead (so
        // two full-scale channels folding together don't come out +6dB
        // hot), run it there, then copy the result back out to every buffer
        // channel so the effect actually reaches the whole width.
        // ponytail: the averaging gain assumes each plugin channel receives
        // the same number of buffer channels (true for the overwhelmingly
        // common case here, mono-in-stereo); an uneven channel count would
        // need a per-target-channel gain instead.
        const auto gain = (float) needed / (float) bufferChannels;

        for (int ch = 0; ch < bufferChannels; ++ch)
            view.addFrom (ch % needed, 0, buffer, ch, 0, n, gain);

        inst.processBlock (view, midi);

        for (int ch = 0; ch < bufferChannels; ++ch)
            buffer.copyFrom (ch, 0, view, ch % needed, 0, n);
    }
};

//==============================================================================
ChannelStrip::ChannelStrip (TrackId id, PluginManager& pm)
    : impl (std::make_unique<Impl> (pm)), trackId (id)
{
}

ChannelStrip::~ChannelStrip() = default;

void ChannelStrip::prepare (double sampleRate, int blockSize, int numChannels)
{
    auto& im = *impl;
    im.sampleRate  = sampleRate > 0.0 ? sampleRate : 44100.0;
    im.blockSize   = juce::jmax (1, blockSize);
    im.numChannels = juce::jlimit (1, maxMeterChannels, numChannels);

    for (auto& fx : im.builtinFx)
        fx->prepare (im.sampleRate, im.blockSize, im.numChannels);

    if (im.instrument != nullptr)
        im.prepareInstance (*im.instrument);

    for (auto& fx : im.pluginFx)
        im.prepareInstance (*fx);

    im.resizeScratch();
}

void ChannelStrip::releaseResources()
{
    auto& im = *impl;

    if (im.instrument != nullptr)
        im.instrument->releaseResources();

    for (auto& fx : im.pluginFx)
        fx->releaseResources();

    for (auto& fx : im.builtinFx)
        fx->reset();
}

void ChannelStrip::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto& im = *impl;
    const auto n  = buffer.getNumSamples();
    const auto nc = buffer.getNumChannels();

    if (n <= 0 || nc <= 0)
        return;

    if (im.instrument != nullptr && n <= im.instrumentScratch.getNumSamples())
    {
        const auto ic = im.instrumentScratch.getNumChannels();
        juce::AudioBuffer<float> view (im.instrumentScratch.getArrayOfWritePointers(), ic, n);
        view.clear();
        im.instrument->processBlock (view, midi);

        // Add rather than replace, so an instrument and audio clips can share a track.
        for (int ch = 0; ch < juce::jmin (nc, ic); ++ch)
            buffer.addFrom (ch, 0, view, ch, 0, n);

        if (ic == 1)
            for (int ch = 1; ch < nc; ++ch)
                buffer.addFrom (ch, 0, view, 0, 0, n);
    }

    for (auto& fx : im.builtinFx)
        if (! fx->bypassed)
            fx->process (buffer);

    for (size_t i = 0; i < im.pluginFx.size(); ++i)
        if (std::atomic_ref<char> (im.pluginBypassed[i]).load (std::memory_order_relaxed) == 0)
            im.runPlugin (*im.pluginFx[i], buffer, midi);

    // --- pan + gain, ramped over the block so knob moves do not click --------
    const auto gain = im.muted.load() ? 0.0f
                                      : juce::Decibels::decibelsToGain (im.gainDb.load());
    const auto pan = juce::jlimit (-1.0f, 1.0f, im.pan.load());
    float l = 1.0f, r = 1.0f;

    /*  A bus is fed by strips that have already been panned, so its own control is
        a balance - unity in the centre.  Running the equal-power law a second time
        would cost 3 dB for nothing more than routing a track through a group.    */
    if (im.balanceLaw)
    {
        l = juce::jmin (1.0f, 1.0f - pan);
        r = juce::jmin (1.0f, 1.0f + pan);
    }
    else
    {
        equalPowerPanGains (pan, l, r);
    }

    const auto newLeft = gain * l, newRight = gain * r;

    if (nc >= 2)
    {
        buffer.applyGainRamp (0, 0, n, im.lastLeft,  newLeft);
        buffer.applyGainRamp (1, 0, n, im.lastRight, newRight);

        for (int ch = 2; ch < nc; ++ch)
            buffer.applyGainRamp (ch, 0, n, im.lastGain, gain);
    }
    else
    {
        buffer.applyGainRamp (0, 0, n, im.lastGain, gain);
    }

    im.lastLeft = newLeft;
    im.lastRight = newRight;
    im.lastGain = gain;

    // --- meters -------------------------------------------------------------
    const auto decay = std::exp (-(float) n / (float) (im.sampleRate * 0.4));

    for (int ch = 0; ch < juce::jmin (nc, maxMeterChannels); ++ch)
    {
        const auto mag = buffer.getMagnitude (ch, 0, n);
        im.peak[(size_t) ch].store (juce::jmax (mag, im.peak[(size_t) ch].load() * decay));
        im.rms[(size_t) ch].store (buffer.getRMSLevel (ch, 0, n));
    }
}

void ChannelStrip::rebuildFrom (const Track& track)
{
    auto& im = *impl;

    /*  Reused-by-identifier pool, keyed by the exact slot.identifier string
        each currently-loaded instance was created from - not re-derived
        from instance->getPluginDescription(), which some plugin types
        round-trip through a different string than the one actually used to
        load them (BasicSynth's fixed identifier vs. its synthesised
        PluginDescription, for one).

        A slot whose identifier is untouched by this rebuild - every slot
        except whichever one was actually added/removed/reordered - keeps
        its live instance, and whatever runtime state it has accumulated
        since it was last loaded, instead of the whole track being torn
        down and every plugin reloaded from a possibly-stale slot.state
        snapshot along with it (this used to make adding one effect to an
        already-loaded chain audibly glitch while every other plugin on the
        track respawned its worker process too). Queued per identifier
        rather than a plain map, so two instances of the same plugin on one
        track are matched one-to-one in their old relative order rather
        than both collapsing onto whichever is found first. Whatever is
        left unclaimed once every new slot has had a chance to match - a
        genuinely removed plugin - is simply destroyed when `reusable`
        goes out of scope. */
    std::map<juce::String, std::deque<std::unique_ptr<juce::AudioPluginInstance>>> reusable;

    if (im.instrument != nullptr)
        reusable[im.instrumentIdentifier].push_back (std::move (im.instrument));

    for (size_t i = 0; i < im.pluginFx.size(); ++i)
        reusable[im.pluginIdentifier[i]].push_back (std::move (im.pluginFx[i]));

    im.instrument.reset();
    im.instrumentIdentifier.clear();
    im.pluginFx.clear();
    im.pluginIdentifier.clear();
    im.pluginBypassed.clear();
    im.pluginSlotIndex.clear();
    im.instrumentSlotIndex = -1;
    im.builtinFx.clear();

    /*  Which slot is the instrument is now stated by the project.  Files written
        before PluginSlot::isInstrument existed flag nothing, so fall back to the
        old rule for those rather than silently losing their instrument.         */
    bool anySlotClaimsInstrument = false;

    for (const auto& slot : track.plugins)
        anySlotClaimsInstrument = anySlotClaimsInstrument || slot.isInstrument;

    for (const auto& slot : track.builtinFx)
    {
        if (auto fx = createBuiltinEffect (slot.type))
        {
            fx->bypassed = slot.bypassed;
            fx->prepare (im.sampleRate, im.blockSize, im.numChannels);
            fx->setParameters (slot.params);
            im.builtinFx.push_back (std::move (fx));
        }
    }

    for (size_t slotIndex = 0; slotIndex < track.plugins.size(); ++slotIndex)
    {
        const auto& slot = track.plugins[slotIndex];

        std::unique_ptr<juce::AudioPluginInstance> instance;

        if (auto it = reusable.find (slot.identifier); it != reusable.end() && ! it->second.empty())
        {
            instance = std::move (it->second.front());
            it->second.pop_front();
        }
        else
        {
            juce::String error;
            instance = im.plugins.createInstance (slot.identifier, im.sampleRate, im.blockSize, error);

            if (instance == nullptr)
            {
                // A missing or blacklisted plugin must not take the track with it -
                // the slot stays in the project so re-installing it restores the chain.
                juce::Logger::writeToLog ("ScoreSmith: " + slot.displayName + " unavailable - " + error);
                continue;
            }

            if (! slot.state.isEmpty())
                instance->setStateInformation (slot.state.getData(), (int) slot.state.getSize());

            im.prepareInstance (*instance);
        }

        bool isInstrument = slot.isInstrument;

        if (! anySlotClaimsInstrument)
        {
            const auto* description = im.plugins.findDescription (slot.identifier);
            isInstrument = description != nullptr && description->isInstrument;
        }

        if (isInstrument && im.instrument == nullptr)
        {
            im.instrument = std::move (instance);
            im.instrumentIdentifier = slot.identifier;
            im.instrumentSlotIndex = (int) slotIndex;
        }
        else
        {
            im.pluginFx.push_back (std::move (instance));
            im.pluginIdentifier.push_back (slot.identifier);
            im.pluginBypassed.push_back (slot.bypassed ? 1 : 0);
            im.pluginSlotIndex.push_back ((int) slotIndex);
        }
    }

    im.resizeScratch();
}

void ChannelStrip::rebuildFrom (const Bus& bus)
{
    auto& im = *impl;

    im.balanceLaw = true;
    im.instrument.reset();
    im.pluginFx.clear();
    im.pluginBypassed.clear();
    im.pluginSlotIndex.clear();
    im.instrumentSlotIndex = -1;
    im.builtinFx.clear();

    for (const auto& slot : bus.builtinFx)
    {
        if (auto fx = createBuiltinEffect (slot.type))
        {
            fx->bypassed = slot.bypassed;
            fx->prepare (im.sampleRate, im.blockSize, im.numChannels);
            fx->setParameters (slot.params);
            im.builtinFx.push_back (std::move (fx));
        }
    }

    im.resizeScratch();
}

void ChannelStrip::setGainDb (float db) noexcept { impl->gainDb.store (db); }
void ChannelStrip::setPan (float p) noexcept     { impl->pan.store (juce::jlimit (-1.0f, 1.0f, p)); }
void ChannelStrip::setMuted (bool m) noexcept    { impl->muted.store (m); }

void ChannelStrip::setPluginBypassed (int slotIndex, bool shouldBypass) noexcept
{
    auto& im = *impl;

    for (size_t i = 0; i < im.pluginSlotIndex.size(); ++i)
        if (im.pluginSlotIndex[i] == slotIndex)
            std::atomic_ref<char> (im.pluginBypassed[i]).store (shouldBypass ? 1 : 0,
                                                                std::memory_order_relaxed);
}

float ChannelStrip::getPeak (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, maxMeterChannels)
             ? impl->peak[(size_t) channel].load() : 0.0f;
}

float ChannelStrip::getRms (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, maxMeterChannels)
             ? impl->rms[(size_t) channel].load() : 0.0f;
}

BuiltinEffect* ChannelStrip::getBuiltinEffect (int index) noexcept
{
    return juce::isPositiveAndBelow (index, (int) impl->builtinFx.size())
             ? impl->builtinFx[(size_t) index].get() : nullptr;
}

juce::AudioPluginInstance* ChannelStrip::getPluginInstance (int index) noexcept
{
    return juce::isPositiveAndBelow (index, (int) impl->pluginFx.size())
             ? impl->pluginFx[(size_t) index].get() : nullptr;
}

juce::AudioPluginInstance* ChannelStrip::getPluginForSlot (int slotIndex) noexcept
{
    auto& im = *impl;

    if (slotIndex >= 0 && slotIndex == im.instrumentSlotIndex)
        return im.instrument.get();

    for (size_t i = 0; i < im.pluginSlotIndex.size(); ++i)
        if (im.pluginSlotIndex[i] == slotIndex)
            return im.pluginFx[i].get();

    return nullptr;
}

juce::AudioPluginInstance* ChannelStrip::getInstrument() noexcept
{
    return impl->instrument.get();
}

//==============================================================================
struct Mixer::Impl
{
    explicit Impl (PluginManager& pm) : pluginManager (pm)
    {
        formatManager.registerBasicFormats();
        readAheadThread.startThread();

        for (auto& v : masterPeak) v.store (0.0f);
        for (auto& v : masterRms)  v.store (0.0f);
    }

    ~Impl()
    {
        trackStates.clear();               // stop the reader thread touching any clip
        readAheadThread.stopThread (2000);
    }

    struct TimedMidi
    {
        juce::int64 sample = 0;
        juce::MidiMessage message;
    };

    /*  One audio clip, wired up on the message thread and pulled by the audio
        thread.  AudioFormatReaderSource -> BufferingAudioSource (does the disk
        read on `readAheadThread`) -> ResamplingAudioSource (file rate and
        playback rate).  The resampler has to be outermost because it is not a
        PositionableAudioSource and so cannot sit under the buffer.

        ponytail: BufferingAudioSource guards its read position with a
        CriticalSection, so the audio thread does take one very short lock per
        seek - the background reader only ever holds it for a pointer swap.  The
        alternative is hand-rolling the read-ahead ring; not worth it until a
        seek shows up in a dropout log.                                         */
    struct ClipPlayback
    {
        juce::int64 startSample = 0, endSample = 0;
        juce::int64 sourceOffsetSamples = 0;
        juce::int64 fadeInSamples = 0, fadeOutSamples = 0;
        juce::int64 nextExpected = -1;
        double ratio = 1.0;
        float  gain = 1.0f;
        bool   reversed = false;

        std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
        std::unique_ptr<juce::BufferingAudioSource> buffering;
        std::unique_ptr<juce::ResamplingAudioSource> resampler;
    };

    /*  One automation lane with its parameter id already resolved to a direct
        target, so the audio thread never parses a string or looks anything up.
        The id convention is shared with the automation editor (spec 8.4.5):
            "gain" | "pan" | "mute"
            "fx:<slotIndex>:<paramId>"
            "plugin:<slotIndex>:<paramIndex>"                                   */
    struct AutoLane
    {
        enum class Target { gain, pan, mute, builtinFx, plugin };

        Target target = Target::gain;
        BuiltinEffect* effect = nullptr;                  // Target::builtinFx
        juce::AudioProcessorParameter* param = nullptr;   // Target::plugin
        int paramIndex = -1;
        std::vector<std::pair<double, float>> points;     // sorted by beat
    };

    struct TrackState
    {
        TrackId id = invalidTrackId;

        /*  Written by setSolo() / setInputMonitoring() while the callback is
            running, so these three cannot be plain bools. */
        std::atomic<bool> soloed { false }, recordArmed { false }, monitoring { false };
        int inputChannel = 0;

        int busIndex = -1;                                // -1 == straight to master
        std::vector<std::pair<int, float>> sends;         // bus index, linear level
        std::vector<AutoLane> lanes;

        juce::String chainSignature, clipSignature;

        std::vector<TimedMidi> midi;
        int midiCursor = 0;
        juce::int64 lastRenderEnd = -1;

        std::vector<std::unique_ptr<ClipPlayback>> clips;

        // --- session clip hand-off (lock-free, one ring buffer per track) ----
        struct SessionRequest
        {
            SessionClip::Kind kind = SessionClip::Kind::midi;
            bool stop = false;
            juce::int64 launchAtSample = 0;
            juce::int64 lengthSamples = 0;
            std::vector<TimedMidi> events;          // kind == midi, built on the message thread
            juce::AudioBuffer<float> audioBuffer;   // kind == audio, loaded on the message thread
        };
        static constexpr int numSessionRequestSlots = 4;   // matches numPreviewSlots' ring depth
        SessionRequest sessionRequests[numSessionRequestSlots];
        std::atomic<int> sessionRequestSlot { -1 };
        std::atomic<int> sessionRequestGeneration { 0 };
        int nextSessionRequestSlot = 0;                     // message thread only

        /*  Queried from the message/UI thread while the audio thread may be
            running concurrently.  Both are single-writer - only Mixer::process()
            (the audio thread) ever assigns them, so a plain atomic is enough
            with no second flag to keep in sync.  (An earlier version added a
            separate `sessionQueued` bool set by BOTH the message thread, at
            publish time, and the audio thread, at adoption time - two writers
            racing could lose an update: the message thread sets it true for a
            brand new request in the exact window between the audio thread
            adopting the previous one and clearing it, and the audio thread's
            clear then wins, permanently hiding a genuinely still-pending
            request from isSessionClipQueued(). Single-writer removes the race.

            `pendingSessionLaunchAtSample` alone still isn't quite enough,
            though: it is audio-thread-only-WRITTEN, but it is only ever
            written from INSIDE Mixer::process() - so immediately after
            launchSessionClip()/stopSessionClip() publish, before process()
            has run even once, it would still read its stale -1 default and
            report "not queued" for a request that unambiguously IS queued.
            isSessionClipQueued() therefore also compares the generation
            counters - `sessionRequestGeneration` (message thread's own
            monotonically-increasing publish counter) against
            `seenSessionGeneration` (the audio thread's own, exactly the same
            already-proven-safe idiom `previewGeneration`/`seenPreviewGeneration`
            uses above for the identical purpose).  Each counter has exactly
            one writer; comparing them for inequality only ever answers
            "is there a published request the audio thread hasn't adopted
            yet", which is safe regardless of interleaving - unlike a shared
            boolean, neither side ever overwrites the other's intent. */
        std::atomic<bool> sessionActive { false };
        std::atomic<juce::int64> pendingSessionLaunchAtSample { -1 };

        // --- audio thread only (seenSessionGeneration is also read from
        //     isSessionClipQueued(), see above - hence atomic) --------------
        std::atomic<int> seenSessionGeneration { 0 };
        int  pendingSessionSlot = -1;          // ring index to adopt once the boundary hits, -1 == none pending
        bool pendingSessionIsStop = false;
        int  activeSessionSlot = -1;           // ring index currently playing
        int  sessionMidiCursor = 0;
        juce::int64 sessionPosSamples = 0;
    };

    static constexpr int numPreviewSlots = 4;

    struct PreviewState
    {
        TrackId track = invalidTrackId;
        juce::int64 lengthSamples = 0;
        std::vector<TimedMidi> events;
    };

    // --- message thread -----------------------------------------------------
    PluginManager& pluginManager;
    Project* project = nullptr;

    double sampleRate = 44100.0;
    int blockSize = 512, numOutputChannels = 2;

    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread readAheadThread { "ScoreSmith clip reader" };

    std::vector<std::unique_ptr<ChannelStrip>> strips;
    std::vector<std::unique_ptr<TrackState>> trackStates;
    std::vector<std::unique_ptr<ChannelStrip>> busStrips;
    std::vector<juce::String> busSignatures;       // parallel to busStrips

    std::vector<std::unique_ptr<BuiltinEffect>> masterFx;
    std::unique_ptr<BuiltinEffect> masterMeter;    // always on, feeds getMasterLufs()

    TempoMap tempo;                                // snapshot taken at rebuild()
    std::atomic<bool> anySoloed { false };

    juce::AudioBuffer<float> trackBuffer, masterBuffer, clipScratch;
    juce::AudioBuffer<float> busBuffers;           // stripChannels per bus, side by side
    juce::MidiBuffer trackMidi;

    std::atomic<float> masterGainDb { 0.0f };
    std::array<std::atomic<float>, maxMeterChannels> masterPeak, masterRms;
    std::atomic<bool> ready { false };

    // --- preview hand-off ---------------------------------------------------
    PreviewState previewSlots[numPreviewSlots];
    std::atomic<int> previewSlot { -1 };
    std::atomic<int> previewGeneration { 0 };
    int nextPreviewSlot = 0;                       // message thread only

    // --- audio thread only --------------------------------------------------
    float lastMasterGain = 1.0f;
    int seenPreviewGeneration = 0, activePreviewSlot = -1, previewCursor = 0;
    juce::int64 previewPos = 0;
    TrackId previewStopTrack = invalidTrackId;

    //==========================================================================
    juce::int64 beatsToSamples (double beats) const noexcept
    {
        return (juce::int64) std::llround (tempo.beatsToSeconds (beats) * sampleRate);
    }

    static juce::String chainSignatureFor (const Track& track)
    {
        juce::String s;

        // Bypass is deliberately NOT in here: it is pushed live through
        // setPluginBypassed(), so toggling it must not re-instantiate the plugin.
        for (const auto& p : track.plugins)
            s << p.identifier << (p.isInstrument ? "|i;" : "|f;");

        s << "//";

        for (const auto& f : track.builtinFx)
            s << f.type << ";";

        return s;
    }

    /*  A rendered UtauClip plays back exactly like an AudioClip once it has a
        renderedFile - rather than teaching ClipPlayback about a second clip
        type, both signature-building and clip-opening below iterate this
        combined view instead of track.audioClips directly. */
    static std::vector<AudioClip> effectiveAudioClipsFor (const Track& track)
    {
        std::vector<AudioClip> clips = track.audioClips;

        for (const auto& u : track.utauClips)
        {
            if (! u.renderedFile.existsAsFile() || u.notesHashAtRender != u.currentContentHash())
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

    static juce::String clipSignatureFor (const Track& track)
    {
        juce::String s;

        for (const auto& c : effectiveAudioClipsFor (track))
            s << c.sourceFile.getFullPathName() << "|" << c.sourceFile.getLastModificationTime().toMilliseconds()
              << "|" << c.startBeats << "|" << c.lengthBeats
              << "|" << c.offsetSeconds << "|" << c.playbackRate << "|" << (c.reversed ? 1 : 0) << ";";

        return s;
    }

    void applyClipGain (ClipPlayback& cp, const AudioClip& clip) const noexcept
    {
        cp.gain = juce::Decibels::decibelsToGain (clip.gainDb);
        cp.fadeInSamples  = (juce::int64) (juce::jmax (0.0, clip.fadeInSec)  * sampleRate);
        cp.fadeOutSamples = (juce::int64) (juce::jmax (0.0, clip.fadeOutSec) * sampleRate);
    }

    void buildMidi (TrackState& state, const Track& track)
    {
        state.midi.clear();

        for (const auto& clip : track.midiClips)
        {
            for (const auto& note : clip.notes)
            {
                if (note.startBeats >= clip.lengthBeats)
                    continue;   // note sits past the end of its clip

                const auto channel  = juce::jlimit (1, 16, note.channel);
                const auto pitch    = juce::jlimit (0, 127, note.pitch);
                const auto velocity = (juce::uint8) juce::jlimit (1, 127, note.velocity);

                const auto onBeat = clip.startBeats + note.startBeats;
                const auto offBeat = clip.startBeats
                                       + juce::jmin (clip.lengthBeats,
                                                     note.startBeats + juce::jmax (0.01, note.lengthBeats));

                state.midi.push_back ({ beatsToSamples (onBeat),
                                        juce::MidiMessage::noteOn (channel, pitch, velocity) });
                state.midi.push_back ({ beatsToSamples (offBeat),
                                        juce::MidiMessage::noteOff (channel, pitch) });
            }
        }

        // stable_sort keeps a note's own off after its on, and an earlier note's
        // off before a same-pitch retrigger at the identical sample.
        std::stable_sort (state.midi.begin(), state.midi.end(),
                          [] (const TimedMidi& a, const TimedMidi& b) { return a.sample < b.sample; });

        state.midiCursor = 0;
        state.lastRenderEnd = -1;
    }

    void buildClips (TrackState& state, const Track& track)
    {
        const auto signature = clipSignatureFor (track);
        const auto effectiveClips = effectiveAudioClipsFor (track);

        if (signature == state.clipSignature && state.clips.size() == effectiveClips.size())
        {
            // Only gain and fades moved - no need to re-open the files.
            for (size_t i = 0; i < state.clips.size(); ++i)
                applyClipGain (*state.clips[i], effectiveClips[i]);

            return;
        }

        state.clips.clear();
        state.clipSignature = signature;

        for (const auto& clip : effectiveClips)
        {
            if (! clip.sourceFile.existsAsFile())
                continue;

            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (clip.sourceFile));

            if (reader == nullptr)
                continue;

            const auto fileRate = reader->sampleRate > 0.0 ? reader->sampleRate : sampleRate;

            auto cp = std::make_unique<ClipPlayback>();
            cp->ratio = (fileRate / sampleRate) * juce::jlimit (0.05, 8.0, clip.playbackRate);
            cp->readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
            cp->buffering = std::make_unique<juce::BufferingAudioSource> (cp->readerSource.get(),
                                                                         readAheadThread, false,
                                                                         (int) (sampleRate * 2.0),
                                                                         stripChannels);
            cp->resampler = std::make_unique<juce::ResamplingAudioSource> (cp->buffering.get(), false,
                                                                          stripChannels);
            cp->resampler->setResamplingRatio (cp->ratio);
            cp->resampler->prepareToPlay (blockSize, sampleRate);

            cp->startSample = beatsToSamples (clip.startBeats);
            cp->endSample   = juce::jmax (cp->startSample + 1, beatsToSamples (clip.endBeats()));
            cp->sourceOffsetSamples = (juce::int64) (juce::jmax (0.0, clip.offsetSeconds) * fileRate);
            cp->reversed = clip.reversed;
            applyClipGain (*cp, clip);

            state.clips.push_back (std::move (cp));
        }
    }

    /** Where the track's main output goes, and its sends, as bus indices. */
    void buildRouting (TrackState& state, const Track& track)
    {
        const auto indexOfBus = [this] (int busId)
        {
            if (busId == 0 || project == nullptr)
                return -1;

            for (int i = 0; i < (int) project->buses.size(); ++i)
                if (project->buses[(size_t) i].id == busId)
                    return i;

            return -1;      // the bus was deleted: fall back to the master
        };

        state.busIndex = indexOfBus (track.outputBus);
        state.sends.clear();

        for (const auto& send : track.sends)
        {
            const auto index = indexOfBus (send.busId);

            if (index >= 0 && send.level > 0.0f)
                state.sends.emplace_back (index, juce::jlimit (0.0f, 1.0f, send.level));
        }
    }

    /*  Resolves each lane's parameter id once, on the message thread, into a
        pointer the audio thread can use directly.  Anything that does not resolve
        (a deleted effect, an id from a newer build) is dropped rather than
        silently automating the wrong thing.                                     */
    void buildAutomation (TrackState& state, const Track& track, ChannelStrip& strip)
    {
        state.lanes.clear();

        for (const auto& lane : track.automation)
        {
            if (lane.points.empty())
                continue;       // a lane with no breakpoints does nothing

            AutoLane resolved;

            if (lane.parameterId == "gain")       resolved.target = AutoLane::Target::gain;
            else if (lane.parameterId == "pan")   resolved.target = AutoLane::Target::pan;
            else if (lane.parameterId == "mute")  resolved.target = AutoLane::Target::mute;
            else if (lane.parameterId.startsWith ("fx:"))
            {
                const auto body    = lane.parameterId.substring (3);
                const auto slot    = body.upToFirstOccurrenceOf (":", false, false).getIntValue();
                const auto paramId = body.fromFirstOccurrenceOf (":", false, false);

                auto* fx = strip.getBuiltinEffect (slot);

                if (fx == nullptr)
                    continue;

                const auto info = fx->getParameterInfo();
                int index = -1;

                for (size_t i = 0; i < info.size(); ++i)
                    if (info[i].id == paramId)
                        index = (int) i;

                if (index < 0)
                    continue;

                resolved.target     = AutoLane::Target::builtinFx;
                resolved.effect     = fx;
                resolved.paramIndex = index;
            }
            else if (lane.parameterId.startsWith ("plugin:"))
            {
                const auto body = lane.parameterId.substring (7);
                const auto slot = body.upToFirstOccurrenceOf (":", false, false).getIntValue();
                const auto paramIndex = body.fromFirstOccurrenceOf (":", false, false).getIntValue();

                auto* instance = strip.getPluginForSlot (slot);

                if (instance == nullptr)
                    continue;

                const auto& params = instance->getParameters();

                if (! juce::isPositiveAndBelow (paramIndex, params.size()))
                    continue;

                resolved.target = AutoLane::Target::plugin;
                resolved.param  = params[paramIndex];
            }
            else
            {
                continue;
            }

            resolved.points = lane.points;
            std::stable_sort (resolved.points.begin(), resolved.points.end(),
                              [] (const auto& a, const auto& b) { return a.first < b.first; });

            state.lanes.push_back (std::move (resolved));
        }
    }

    static juce::String busSignatureFor (const Bus& bus)
    {
        juce::String s;
        s << bus.id << "/";

        for (const auto& f : bus.builtinFx)
            s << f.type << ";";

        return s;
    }

    void rebuildMasterChain (const std::vector<BuiltinFxSlot>& chain)
    {
        masterFx.clear();

        for (const auto& slot : chain)
        {
            if (auto fx = createBuiltinEffect (slot.type))
            {
                fx->bypassed = slot.bypassed;
                fx->prepare (sampleRate, blockSize, stripChannels);
                fx->setParameters (slot.params);
                masterFx.push_back (std::move (fx));
            }
        }

        if (masterMeter == nullptr)
        {
            masterMeter = createBuiltinEffect ("loudnessMeter");

            if (masterMeter != nullptr)
                masterMeter->prepare (sampleRate, blockSize, stripChannels);
        }
    }

    /*  ponytail: the tempo is snapshotted once per rebuild and pushed to the
        synced effects then.  A delay riding a tempo *ramp* will hold the tempo at
        the ramp's start until the next rebuild; push it per block off the tempo
        snapshot if that ever shows up.                                          */
    void pushTempo()
    {
        const auto bpm = tempo.bpmAt (0.0);

        const auto pushToStrip = [bpm] (ChannelStrip& strip)
        {
            for (int i = 0; ; ++i)
            {
                auto* fx = strip.getBuiltinEffect (i);

                if (fx == nullptr)
                    break;

                fx->setTempoBpm (bpm);
            }
        };

        for (auto& strip : strips)    pushToStrip (*strip);
        for (auto& strip : busStrips) pushToStrip (*strip);

        for (auto& fx : masterFx)
            fx->setTempoBpm (bpm);
    }

    //== audio thread ==========================================================
    /*  ponytail: automation is read-only - a lane always wins over the fader for
        its block, and there is no write/touch/latch mode.  Add a per-lane mode to
        Track::AutomationLane when the editor grows record arming.               */
    void applyAutomation (TrackState& state, ChannelStrip& strip, double beat) noexcept
    {
        for (auto& lane : state.lanes)
        {
            const auto v = automationValueAt (lane.points, beat, 0.0f);

            switch (lane.target)
            {
                case AutoLane::Target::gain:   strip.setGainDb (-60.0f + 66.0f * v); break;
                case AutoLane::Target::pan:    strip.setPan (v * 2.0f - 1.0f);       break;
                case AutoLane::Target::mute:   strip.setMuted (v >= 0.5f);           break;

                case AutoLane::Target::builtinFx:
                    if (lane.effect != nullptr)
                        lane.effect->setParameterNormalised (lane.paramIndex, v);
                    break;

                case AutoLane::Target::plugin:
                    // AudioProcessorParameter::setValue is the host->plugin path and
                    // is required to be callable from the audio thread.
                    if (lane.param != nullptr)
                        lane.param->setValue (juce::jlimit (0.0f, 1.0f, v));
                    break;
            }
        }
    }

    /** [audio thread] Adds `source` into bus `index`, or into `master` when < 0. */
    void addToDestination (int index, const juce::AudioBuffer<float>& source, int numSamples,
                           juce::AudioBuffer<float>& master, float gain) noexcept
    {
        for (int ch = 0; ch < stripChannels; ++ch)
        {
            if (index < 0)
                master.addFrom (ch, 0, source, ch, 0, numSamples, gain);
            else
                busBuffers.addFrom (index * stripChannels + ch, 0, source, ch, 0, numSamples, gain);
        }
    }

    void gatherMidi (TrackState& state, juce::int64 position, int numSamples, juce::MidiBuffer& dest)
    {
        if (state.lastRenderEnd != position)
        {
            // A seek, a loop wrap or the first block after play: re-find our place
            // and kill anything the instrument is still holding.
            addAllNotesOff (dest);

            state.midiCursor = (int) (std::lower_bound (state.midi.begin(), state.midi.end(), position,
                                          [] (const TimedMidi& e, juce::int64 s) { return e.sample < s; })
                                      - state.midi.begin());
        }

        const auto blockEnd = position + numSamples;

        while (state.midiCursor < (int) state.midi.size()
                && state.midi[(size_t) state.midiCursor].sample < blockEnd)
        {
            const auto& e = state.midi[(size_t) state.midiCursor];
            dest.addEvent (e.message, (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1,
                                                          e.sample - position));
            ++state.midiCursor;
        }

        state.lastRenderEnd = blockEnd;
    }

    void renderClips (TrackState& state, juce::int64 position, int numSamples,
                      juce::AudioBuffer<float>& dest)
    {
        const auto blockEnd = position + numSamples;

        for (auto& clipPtr : state.clips)
        {
            auto& cp = *clipPtr;

            if (cp.endSample <= position || cp.startSample >= blockEnd)
            {
                cp.nextExpected = -1;
                continue;
            }

            const auto from = juce::jmax (position, cp.startSample);
            const auto to   = juce::jmin (blockEnd, cp.endSample);
            const auto numToRead = (int) (to - from);

            if (numToRead <= 0 || numToRead > clipScratch.getNumSamples())
                continue;

            /*  ponytail: a reversed clip seeks backwards every block, which defeats
                the read-ahead buffer.  Bake a reversed copy to disk on the message
                thread if reversed playback ever stutters.                          */
            if (cp.reversed || cp.nextExpected != from)
            {
                const auto elapsed = cp.reversed ? (cp.endSample - to) : (from - cp.startSample);
                const auto filePos = cp.sourceOffsetSamples + (juce::int64) ((double) elapsed * cp.ratio);

                cp.buffering->setNextReadPosition (juce::jmax ((juce::int64) 0, filePos));
                cp.resampler->flushBuffers();
            }

            clipScratch.clear (0, numToRead);

            juce::AudioSourceChannelInfo info;
            info.buffer = &clipScratch;
            info.startSample = 0;
            info.numSamples = numToRead;
            cp.resampler->getNextAudioBlock (info);

            if (cp.reversed)
                for (int ch = 0; ch < clipScratch.getNumChannels(); ++ch)
                {
                    auto* d = clipScratch.getWritePointer (ch);
                    std::reverse (d, d + numToRead);
                }

            cp.nextExpected = cp.reversed ? -1 : to;

            const auto destOffset = (int) (from - position);
            const auto elapsedAtStart = from - cp.startSample;
            const auto clipLength = cp.endSample - cp.startSample;

            for (int ch = 0; ch < juce::jmin (dest.getNumChannels(), clipScratch.getNumChannels()); ++ch)
            {
                const auto* src = clipScratch.getReadPointer (ch);
                auto* out = dest.getWritePointer (ch);

                for (int i = 0; i < numToRead; ++i)
                {
                    const auto elapsed = elapsedAtStart + i;
                    auto g = cp.gain;

                    if (cp.fadeInSamples > 0 && elapsed < cp.fadeInSamples)
                        g *= (float) elapsed / (float) cp.fadeInSamples;

                    if (cp.fadeOutSamples > 0 && elapsed > clipLength - cp.fadeOutSamples)
                        g *= (float) (clipLength - elapsed) / (float) cp.fadeOutSamples;

                    out[destOffset + i] += src[i] * g;
                }
            }
        }
    }

    void gatherPreview (int numSamples, juce::MidiBuffer& dest)
    {
        const auto& pv = previewSlots[activePreviewSlot];
        const auto blockEnd = previewPos + numSamples;

        while (previewCursor < (int) pv.events.size()
                && pv.events[(size_t) previewCursor].sample < blockEnd)
        {
            const auto& e = pv.events[(size_t) previewCursor];
            dest.addEvent (e.message, (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1,
                                                          e.sample - previewPos));
            ++previewCursor;
        }
    }

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

    // ponytail: sessionMidiCursor only resets between blocks, so EVERY loop
    // wrap quantizes the new iteration's opening events to the start of the
    // next audio block - up to ~10ms of jitter at typical block sizes, on
    // every wrap, not just for short clips.  A clip shorter than one block
    // additionally DROPS events (a whole loop iteration, sometimes more, can
    // elapse between two cursor resets).  Real session clips are always at
    // least a beat long, vastly longer than one block, so the drop case is
    // not reachable in practice; a sub-block-accurate rewrite would be needed
    // only if sub-block-length clips become a real use case.
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
};

//==============================================================================
Mixer::Mixer (PluginManager& pm) : impl (std::make_unique<Impl> (pm)) {}
Mixer::~Mixer() = default;

void Mixer::setProject (Project* p)
{
    impl->ready.store (false);
    impl->project = p;
    impl->strips.clear();
    impl->trackStates.clear();
    rebuild();
}

void Mixer::prepare (double sampleRate, int blockSize, int numOutputChannels)
{
    auto& im = *impl;
    im.ready.store (false);

    im.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    im.blockSize  = juce::jmax (1, blockSize);
    im.numOutputChannels = juce::jmax (1, numOutputChannels);

    im.trackBuffer .setSize (stripChannels, im.blockSize, false, true, false);
    im.masterBuffer.setSize (stripChannels, im.blockSize, false, true, false);
    im.clipScratch .setSize (stripChannels, im.blockSize, false, true, false);
    im.trackMidi.ensureSize (8192);

    for (auto& strip : im.strips)
        strip->prepare (im.sampleRate, im.blockSize, stripChannels);

    for (auto& strip : im.busStrips)
        strip->prepare (im.sampleRate, im.blockSize, stripChannels);

    im.busSignatures.clear();     // force the bus chains to be made again

    // Everything downstream of the sample rate has to be made again.
    for (auto& state : im.trackStates)
    {
        state->chainSignature.clear();
        state->clipSignature.clear();
    }

    im.masterMeter.reset();
    im.lastMasterGain = juce::Decibels::decibelsToGain (im.masterGainDb.load());

    rebuild();
}

void Mixer::releaseResources()
{
    auto& im = *impl;
    im.ready.store (false);

    for (auto& strip : im.strips)
        strip->releaseResources();

    for (auto& strip : im.busStrips)
        strip->releaseResources();

    for (auto& fx : im.masterFx)
        fx->reset();

    im.trackStates.clear();     // releases the clip readers and the buffering sources
}

void Mixer::rebuild()
{
    auto& im = *impl;
    im.ready.store (false);

    if (im.project == nullptr)
    {
        im.strips.clear();
        im.trackStates.clear();
        im.busStrips.clear();
        im.busSignatures.clear();
        im.rebuildMasterChain ({});
        return;
    }

    im.tempo = im.project->tempo;
    im.anySoloed.store (im.project->anyTrackSoloed());

    std::vector<std::unique_ptr<ChannelStrip>> newStrips;
    std::vector<std::unique_ptr<Impl::TrackState>> newStates;
    newStrips.reserve ((size_t) im.project->getNumTracks());
    newStates.reserve ((size_t) im.project->getNumTracks());

    for (int i = 0; i < im.project->getNumTracks(); ++i)
    {
        auto& track = im.project->getTrack (i);
        const auto id = track.getId();

        std::unique_ptr<ChannelStrip> strip;
        std::unique_ptr<Impl::TrackState> state;

        for (auto& existing : im.strips)
            if (existing != nullptr && existing->getTrackId() == id)
                strip = std::move (existing);

        for (auto& existing : im.trackStates)
            if (existing != nullptr && existing->id == id)
                state = std::move (existing);

        if (strip == nullptr)
        {
            strip = std::make_unique<ChannelStrip> (id, im.pluginManager);
            strip->prepare (im.sampleRate, im.blockSize, stripChannels);
        }

        if (state == nullptr)
        {
            state = std::make_unique<Impl::TrackState>();
            state->id = id;
        }

        // Only reload plugins when the chain itself changed - a solo click or a
        // clip move must not re-instantiate every VST on the track.
        const auto signature = Impl::chainSignatureFor (track);

        if (state->chainSignature != signature)
        {
            strip->rebuildFrom (track);
            state->chainSignature = signature;
        }

        for (size_t f = 0; f < track.builtinFx.size(); ++f)
        {
            if (auto* fx = strip->getBuiltinEffect ((int) f))
            {
                fx->bypassed = track.builtinFx[f].bypassed;
                fx->setParameters (track.builtinFx[f].params);
            }
        }

        for (size_t p = 0; p < track.plugins.size(); ++p)
            strip->setPluginBypassed ((int) p, track.plugins[p].bypassed);

        strip->setGainDb (track.gainDb);
        strip->setPan (track.pan);
        strip->setMuted (track.muted);

        state->soloed.store (track.soloed);
        state->recordArmed.store (track.recordArmed);
        state->monitoring.store (track.inputMonitoring);
        state->inputChannel = track.inputChannel;

        im.buildRouting (*state, track);
        im.buildAutomation (*state, track, *strip);
        im.buildMidi (*state, track);
        im.buildClips (*state, track);

        newStrips.push_back (std::move (strip));
        newStates.push_back (std::move (state));
    }

    im.strips = std::move (newStrips);
    im.trackStates = std::move (newStates);

    // --- bus strips (spec 8.4.5) --------------------------------------------
    std::vector<std::unique_ptr<ChannelStrip>> newBusStrips;
    std::vector<juce::String> newBusSignatures;

    for (size_t b = 0; b < im.project->buses.size(); ++b)
    {
        const auto& bus = im.project->buses[b];
        const auto signature = Impl::busSignatureFor (bus);

        std::unique_ptr<ChannelStrip> strip;

        // Keep the existing strip when the chain is unchanged, so editing an
        // unrelated track does not cut a bus reverb's tail.
        if (b < im.busStrips.size() && b < im.busSignatures.size()
             && im.busSignatures[b] == signature)
            strip = std::move (im.busStrips[b]);

        if (strip == nullptr)
        {
            strip = std::make_unique<ChannelStrip> (invalidTrackId, im.pluginManager);
            strip->prepare (im.sampleRate, im.blockSize, stripChannels);
            strip->rebuildFrom (bus);
        }

        for (size_t f = 0; f < bus.builtinFx.size(); ++f)
        {
            if (auto* fx = strip->getBuiltinEffect ((int) f))
            {
                fx->bypassed = bus.builtinFx[f].bypassed;
                fx->setParameters (bus.builtinFx[f].params);
            }
        }

        strip->setGainDb (bus.gainDb);
        strip->setPan (bus.pan);
        strip->setMuted (bus.muted);

        newBusStrips.push_back (std::move (strip));
        newBusSignatures.push_back (signature);
    }

    im.busStrips = std::move (newBusStrips);
    im.busSignatures = std::move (newBusSignatures);

    // Allocated here, on the message thread with the callback suspended - the
    // block loop only ever takes views over this storage.
    im.busBuffers.setSize (juce::jmax (stripChannels, (int) im.busStrips.size() * stripChannels),
                           im.blockSize, false, true, false);

    im.rebuildMasterChain (im.project->masterChain);
    im.pushTempo();
    im.ready.store (true);
}

void Mixer::syncClips()
{
    auto& im = *impl;

    if (im.project == nullptr)
        return;

    im.ready.store (false);

    for (auto& state : im.trackStates)
        if (auto* track = im.project->findTrack (state->id))
            im.buildClips (*state, *track);

    im.ready.store (true);
}

void Mixer::setSolo (TrackId id, bool soloed) noexcept
{
    auto& im = *impl;
    bool any = false;

    for (auto& state : im.trackStates)
    {
        if (state == nullptr)
            continue;

        if (state->id == id)
            state->soloed.store (soloed);

        any = any || state->soloed.load();
    }

    im.anySoloed.store (any);
}

void Mixer::setInputMonitoring (TrackId id, bool recordArmed, bool monitoring) noexcept
{
    for (auto& state : impl->trackStates)
        if (state != nullptr && state->id == id)
        {
            state->recordArmed.store (recordArmed);
            state->monitoring.store (monitoring);
        }
}

void Mixer::setPluginBypassed (TrackId id, int slotIndex, bool bypassed) noexcept
{
    if (auto* strip = getStripForTrack (id))
        strip->setPluginBypassed (slotIndex, bypassed);
}

void Mixer::process (juce::AudioBuffer<float>& output, juce::int64 positionSamples,
                     juce::MidiBuffer& liveMidi, const juce::AudioBuffer<float>& deviceInput,
                     bool isPlaying, juce::int64 sessionPositionSamples)
{
    const juce::ScopedNoDenormals noDenormals;

    auto& im = *impl;
    const auto n = output.getNumSamples();
    output.clear();

    if (! im.ready.load() || n <= 0 || n > im.blockSize)
        return;

    const bool rolling = isPlaying;

    // Automation is evaluated once per block, at the block's own position.
    const auto blockBeat = im.tempo.secondsToBeats ((double) positionSamples / im.sampleRate);

    // --- preview hand-off (atomic slot swap, no lock) -----------------------
    const auto generation = im.previewGeneration.load (std::memory_order_acquire);

    if (generation != im.seenPreviewGeneration)
    {
        im.seenPreviewGeneration = generation;
        im.previewStopTrack = im.activePreviewSlot >= 0
                                ? im.previewSlots[im.activePreviewSlot].track : invalidTrackId;
        im.activePreviewSlot = im.previewSlot.load (std::memory_order_acquire);
        im.previewPos = 0;
        im.previewCursor = 0;
    }

    /*  One AudioBuffer object per block over the master storage: JUCE tracks an
        `isClear` flag per AudioBuffer, so summing through the member buffer while
        the FX chain writes through a second view of the same memory would let a
        stale reverb tail survive the next clear().                              */
    juce::AudioBuffer<float> master (im.masterBuffer.getArrayOfWritePointers(), stripChannels, n);
    master.clear();

    const auto numBuses = juce::jmin ((int) im.busStrips.size(),
                                      im.busBuffers.getNumChannels() / stripChannels);

    for (int ch = 0; ch < numBuses * stripChannels; ++ch)
        im.busBuffers.clear (ch, 0, n);

    for (size_t t = 0; t < im.strips.size() && t < im.trackStates.size(); ++t)
    {
        auto& state = *im.trackStates[t];
        auto& strip = *im.strips[t];

        juce::AudioBuffer<float> track (im.trackBuffer.getArrayOfWritePointers(), stripChannels, n);
        track.clear();
        im.trackMidi.clear();

        const bool previewing = im.activePreviewSlot >= 0
                                  && im.previewSlots[im.activePreviewSlot].track == state.id;

        if (state.id == im.previewStopTrack)
            addAllNotesOff (im.trackMidi);

        if (rolling)
        {
            im.gatherMidi (state, positionSamples, n, im.trackMidi);
            im.renderClips (state, positionSamples, n, track);
        }
        else if (state.lastRenderEnd != -1)
        {
            addAllNotesOff (im.trackMidi);
            state.lastRenderEnd = -1;

            for (auto& clip : state.clips)
                clip->nextExpected = -1;
        }

        const bool armed = state.recordArmed.load();

        if (armed && ! liveMidi.isEmpty())
            im.trackMidi.addEvents (liveMidi, 0, n, 0);

        /*  Input monitoring (spec 8.4.3): the hardware input joins the track here,
            so it is heard through this track's own FX chain, fader and pan rather
            than tapped off raw.                                                 */
        if (armed && state.monitoring.load()
             && juce::isPositiveAndBelow (state.inputChannel, deviceInput.getNumChannels())
             && n <= deviceInput.getNumSamples())
        {
            for (int ch = 0; ch < stripChannels; ++ch)
                track.addFrom (ch, 0, deviceInput, state.inputChannel, 0, n);
        }

        if (previewing)
            im.gatherPreview (n, im.trackMidi);

        // --- session clip hand-off (lock-free, per track) --------------------
        // Boundaries are compared against `sessionPositionSamples` - SessionClock's
        // OWN free-running clock - never `positionSamples`, which is Transport's
        // position and freezes while stopped, jumps on seek and can move backwards
        // on a loop.  A Session cell must launch/stop on its own clock regardless
        // of what Transport is doing (see SessionClock.h's own doc comment).
        //
        // `sessionPositionSamples < 0` is the sentinel meaning "this call has no
        // real SessionClock position" (offline export - see Mixer::process's doc
        // comment): session state is neither adopted/replaced nor advanced, and
        // nothing session-related is rendered, for that call.  Without this an
        // export would bake a live session clip into the file at a bogus phase
        // AND leave the live loop's position scrambled once the export finished.
        if (sessionPositionSamples >= 0)
        {
            const auto sessionGen = state.sessionRequestGeneration.load (std::memory_order_acquire);

            if (sessionGen != state.seenSessionGeneration.load())
            {
                state.pendingSessionSlot   = state.sessionRequestSlot.load (std::memory_order_acquire);
                state.pendingSessionIsStop = state.sessionRequests[(size_t) state.pendingSessionSlot].stop;
                state.pendingSessionLaunchAtSample.store (
                    state.sessionRequests[(size_t) state.pendingSessionSlot].launchAtSample);

                // Stored LAST, after pendingSessionLaunchAtSample: isSessionClipQueued()
                // (message thread) treats "generations differ" OR "a launch sample is
                // pending" as queued.  Storing seenSessionGeneration first would open a
                // window, between these two stores, where BOTH read false for a request
                // that is unambiguously still queued - self-correcting on the very next
                // poll, but there is no reason to leave the window open when a pure
                // statement reorder closes it for free.
                state.seenSessionGeneration.store (sessionGen);
            }

            const auto pendingLaunchAtSample = state.pendingSessionLaunchAtSample.load();

            if (pendingLaunchAtSample >= 0 && sessionPositionSamples + n >= pendingLaunchAtSample)
            {
                if (state.pendingSessionIsStop)
                {
                    // Whatever this clip left sounding must be released now - the
                    // engine gets no other chance to until Transport starts/stops
                    // (identical defect, and identical fix, to the loop-wrap case
                    // below - a stopped session clip used to leave notes hanging
                    // exactly like an unlucky loop wrap did).
                    addAllNotesOff (im.trackMidi);
                    state.sessionActive.store (false);
                }
                else
                {
                    // A replace (a different clip's slot adopted while this track's
                    // session was already active - e.g. switching candidate scenes,
                    // Task 8's primary workflow) swaps activeSessionSlot without ever
                    // visiting the OLD clip's own note-offs.  Release whatever was
                    // sounding before adopting the new slot; harmless no-op the rest
                    // of the time (nothing was playing yet).
                    addAllNotesOff (im.trackMidi);
                    state.activeSessionSlot = state.pendingSessionSlot;
                    state.sessionPosSamples = 0;
                    state.sessionMidiCursor = 0;
                    state.sessionActive.store (true);
                }

                state.pendingSessionLaunchAtSample.store (-1);
                state.pendingSessionSlot = -1;
            }

            if (state.sessionActive.load() && state.activeSessionSlot >= 0)
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

                    /*  A note-off clamped to exactly `lengthSamples` fails
                        gatherSession's strict `sample < blockEnd` check whenever a
                        render block happens to end exactly on the loop boundary
                        (reachable at ordinary settings, not just contrived ones),
                        so it would otherwise never be delivered and the note would
                        hang forever.  Forcing all-notes-off here means a loop
                        iteration can never hand a stuck note to the next one,
                        regardless of exact sample alignment. */
                    if (activeReq.kind == SessionClip::Kind::midi)
                        addAllNotesOff (im.trackMidi);
                }
            }
        }

        im.applyAutomation (state, strip, blockBeat);
        strip.process (track, im.trackMidi);

        if (! (previewing || ! im.anySoloed.load() || state.soloed.load()))
            continue;

        im.addToDestination (state.busIndex < numBuses ? state.busIndex : -1,
                             track, n, master, 1.0f);

        // Post-fader sends, on top of the main path (spec 8.4.5).
        for (const auto& send : state.sends)
            if (send.first < numBuses)
                im.addToDestination (send.first, track, n, master, send.second);
    }

    /*  Buses sum into the master in project order.
        ponytail: a bus can only feed the master.  Bus -> bus would need the buses
        sorted topologically here (and a cycle check in Project); 8.4.5 asks for
        groups and sends, not a routing matrix.                                  */
    for (int b = 0; b < numBuses; ++b)
    {
        juce::AudioBuffer<float> bus (im.busBuffers.getArrayOfWritePointers() + b * stripChannels,
                                      stripChannels, n);
        im.trackMidi.clear();
        im.busStrips[(size_t) b]->process (bus, im.trackMidi);

        for (int ch = 0; ch < stripChannels; ++ch)
            master.addFrom (ch, 0, bus, ch, 0, n);
    }

    im.previewStopTrack = invalidTrackId;

    if (im.activePreviewSlot >= 0)
    {
        im.previewPos += n;

        if (im.previewPos > im.previewSlots[im.activePreviewSlot].lengthSamples)
            im.activePreviewSlot = -1;
    }

    // --- master chain -------------------------------------------------------
    for (auto& fx : im.masterFx)
        if (! fx->bypassed)
            fx->process (master);

    const auto masterGain = juce::Decibels::decibelsToGain (im.masterGainDb.load());

    for (int ch = 0; ch < stripChannels; ++ch)
        master.applyGainRamp (ch, 0, n, im.lastMasterGain, masterGain);

    im.lastMasterGain = masterGain;

    if (im.masterMeter != nullptr)
        im.masterMeter->process (master);

    const auto decay = std::exp (-(float) n / (float) (im.sampleRate * 0.4));

    for (int ch = 0; ch < stripChannels; ++ch)
    {
        const auto mag = master.getMagnitude (ch, 0, n);
        im.masterPeak[(size_t) ch].store (juce::jmax (mag, im.masterPeak[(size_t) ch].load() * decay));
        im.masterRms[(size_t) ch].store (master.getRMSLevel (ch, 0, n));
    }

    const auto outChannels = output.getNumChannels();

    if (outChannels == 1)
    {
        output.addFrom (0, 0, master, 0, 0, n, 0.5f);   // output was cleared above
        output.addFrom (0, 0, master, 1, 0, n, 0.5f);
    }
    else
    {
        for (int ch = 0; ch < juce::jmin (outChannels, stripChannels); ++ch)
            output.copyFrom (ch, 0, master, ch, 0, n);
    }
}

ChannelStrip* Mixer::getStripForTrack (TrackId id) noexcept
{
    for (auto& strip : impl->strips)
        if (strip != nullptr && strip->getTrackId() == id)
            return strip.get();

    return nullptr;
}

ChannelStrip* Mixer::getBusStrip (int index) noexcept
{
    return juce::isPositiveAndBelow (index, (int) impl->busStrips.size())
             ? impl->busStrips[(size_t) index].get() : nullptr;
}

BuiltinEffect* Mixer::getMasterEffect (int index) noexcept
{
    return juce::isPositiveAndBelow (index, (int) impl->masterFx.size())
             ? impl->masterFx[(size_t) index].get() : nullptr;
}

float Mixer::getMasterPeak (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, maxMeterChannels)
             ? impl->masterPeak[(size_t) channel].load() : 0.0f;
}

float Mixer::getMasterRms (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, maxMeterChannels)
             ? impl->masterRms[(size_t) channel].load() : 0.0f;
}

float Mixer::getMasterTruePeakDb() const noexcept
{
    /*  The inter-sample peak, from the same always-on BS.1770 meter that feeds
        getMasterLufs().  It is what a delivery spec means by "peak", and it can
        sit well above the sample peak on a limited master.                     */
    return impl->masterMeter != nullptr ? impl->masterMeter->getMeter ("truePeak") : -100.0f;
}

float Mixer::getMasterLufs() const noexcept
{
    // Integrated: this is the number the streaming loudness presets are matched
    // against.  Momentary and short-term come off the same always-on meter, as
    // does getMasterTruePeakDb().
    return impl->masterMeter != nullptr ? impl->masterMeter->getMeter ("integrated") : -100.0f;
}

void Mixer::resetLoudness()
{
    if (impl->masterMeter != nullptr)
        impl->masterMeter->reset();
}

void Mixer::setMasterGainDb (float db) noexcept  { impl->masterGainDb.store (db); }
float Mixer::getMasterGainDb() const noexcept    { return impl->masterGainDb.load(); }

void Mixer::setPreviewClip (TrackId trackId, const MidiClip* clip)
{
    auto& im = *impl;

    if (clip == nullptr)
    {
        im.previewSlot.store (-1, std::memory_order_release);
        im.previewGeneration.fetch_add (1, std::memory_order_release);
        return;
    }

    /*  ponytail: four rotating slots rather than a full RCU hand-off.  Auditioning
        four different clips inside a single audio block would race; nobody clicks
        that fast.  Move to a lock-free queue of owned states if it ever matters.  */
    const auto slot = im.nextPreviewSlot;
    im.nextPreviewSlot = (im.nextPreviewSlot + 1) % Impl::numPreviewSlots;

    auto& preview = im.previewSlots[slot];
    preview.track = trackId;
    preview.events.clear();

    // The audition always starts at zero - the timeline is not moved (spec 9.4).
    for (const auto& note : clip->notes)
    {
        if (note.startBeats >= clip->lengthBeats)
            continue;

        const auto channel  = juce::jlimit (1, 16, note.channel);
        const auto pitch    = juce::jlimit (0, 127, note.pitch);
        const auto velocity = (juce::uint8) juce::jlimit (1, 127, note.velocity);
        const auto offBeat  = juce::jmin (clip->lengthBeats,
                                          note.startBeats + juce::jmax (0.01, note.lengthBeats));

        preview.events.push_back ({ im.beatsToSamples (note.startBeats),
                                    juce::MidiMessage::noteOn (channel, pitch, velocity) });
        preview.events.push_back ({ im.beatsToSamples (offBeat),
                                    juce::MidiMessage::noteOff (channel, pitch) });
    }

    std::stable_sort (preview.events.begin(), preview.events.end(),
                      [] (const Impl::TimedMidi& a, const Impl::TimedMidi& b) { return a.sample < b.sample; });

    // Half a second of tail so release stages are not chopped off.
    preview.lengthSamples = im.beatsToSamples (clip->lengthBeats) + (juce::int64) (im.sampleRate * 0.5);

    im.previewSlot.store (slot, std::memory_order_release);
    im.previewGeneration.fetch_add (1, std::memory_order_release);
}

void Mixer::launchSessionClip (TrackId trackId, const SessionClip& clip, juce::int64 launchAtSample)
{
    auto& im = *impl;

    for (auto& statePtr : im.trackStates)
    {
        if (statePtr->id != trackId)
            continue;

        auto& state = *statePtr;

        /*  ponytail: four rotating slots per track (numSessionRequestSlots),
            same trade-off as setPreviewClip's ring above - the 4th NEW
            launch/stop published for this track, before the audio thread has
            adopted even the first of them, wraps back onto that first slot
            and overwrites it.  Move to a lock-free queue of owned states if
            that ever shows up in practice.

            Unlike setPreviewClip's ring, though, a session slot is not
            necessarily retired quickly: `activeSessionSlot` can point at one
            slot for as long as that clip keeps looping (potentially minutes),
            not just the fraction of a second an audition lasts.  If 4+
            launch/stop calls for the SAME track ever land within one audio
            block while a slot is still the audio thread's active/pending one
            (not reachable today - a scene fire issues at most one call per
            track per user action), the overwrite is a live vector/buffer
            being mutated out from under gatherSession/renderSessionAudio
            mid-read, not just a stale read - a real race, deliberately not
            solved here. */
        const auto slot = state.nextSessionRequestSlot;
        state.nextSessionRequestSlot = (state.nextSessionRequestSlot + 1) % Impl::TrackState::numSessionRequestSlots;

        auto& req = state.sessionRequests[(size_t) slot];
        req.kind = clip.kind;
        req.stop = false;
        req.launchAtSample = launchAtSample;

        if (clip.kind == SessionClip::Kind::midi)
        {
            req.events.clear();

            for (const auto& note : clip.notes)
            {
                if (note.startBeats >= clip.lengthBeats)
                    continue;   // note sits past the end of its clip

                const auto channel  = juce::jlimit (1, 16, note.channel);
                const auto pitch    = juce::jlimit (0, 127, note.pitch);
                const auto velocity = (juce::uint8) juce::jlimit (1, 127, note.velocity);
                const auto offBeat  = juce::jmin (clip.lengthBeats,
                                                  note.startBeats + juce::jmax (0.01, note.lengthBeats));

                req.events.push_back ({ im.beatsToSamples (note.startBeats),
                    juce::MidiMessage::noteOn (channel, pitch, velocity) });
                req.events.push_back ({ im.beatsToSamples (offBeat),
                    juce::MidiMessage::noteOff (channel, pitch) });
            }

            std::stable_sort (req.events.begin(), req.events.end(),
                              [] (const Impl::TimedMidi& a, const Impl::TimedMidi& b) { return a.sample < b.sample; });

            req.lengthSamples = im.beatsToSamples (clip.lengthBeats);
        }
        else
        {
            // A missing/empty source is a valid, silent, zero-length clip -
            // not an error - so "active" always reflects scheduling state.
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

        auto& req = state.sessionRequests[(size_t) slot];
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
            return statePtr->sessionActive.load();

    return false;
}

bool Mixer::isSessionClipQueued (TrackId trackId) const noexcept
{
    for (auto& statePtr : impl->trackStates)
        if (statePtr->id == trackId)
            return statePtr->sessionRequestGeneration.load() != statePtr->seenSessionGeneration.load()
                     || statePtr->pendingSessionLaunchAtSample.load() >= 0;

    return false;
}

}
