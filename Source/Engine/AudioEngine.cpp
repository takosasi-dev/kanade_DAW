#include "Engine/AudioEngine.h"
#include "Mixer/Mixer.h"
#include "Plugins/PluginManager.h"
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int   offlineBlockSize   = 512;
        constexpr float meterDecayPerBlock = 0.85f;

        /** Everything that forces the mixer graph to be rebuilt.  Clip edits are not in
            here on purpose - the mixer reads clips from the project as it plays. */
        juce::String topologyFingerprint (const Project& project)
        {
            juce::String description;

            for (const auto& track : project.getTracks())
            {
                description << track->getId() << '|' << (int) track->getType() << '|'
                            << track->inputChannel << '|' << track->outputBus << '|'
                            << track->midiInputDevice << ';';

                // Plugin bypass, record arm and input monitoring are deliberately
                // absent: pushMixerState() pushes them live, and rebuilding here
                // would re-open every clip reader for a button press.
                for (const auto& plugin : track->plugins)
                    description << plugin.identifier << ',' << (plugin.isInstrument ? 'i' : 'f') << ';';

                for (const auto& fx : track->builtinFx)
                    description << fx.type << ',' << (fx.bypassed ? '1' : '0') << ';';

                for (const auto& send : track->sends)
                    description << 's' << send.busId << ',' << send.level << ';';

                /*  The breakpoint positions have to be in here, not just the
                    count: the mixer resolves each lane into its own copy at
                    rebuild time, so moving a point - which leaves the count
                    identical - would otherwise never reach the audio thread and
                    the curve would keep playing back as it was first drawn.

                    ponytail: this makes one gesture cost a full graph rebuild
                    (every clip reader re-opened).  Acceptable because the editor
                    commits once on mouse-up, not per mouse-move; if automation
                    ever gets a live-drag or record mode, give Mixer a
                    double-buffered lane swap and take automation back out of
                    this fingerprint.                                          */
                for (const auto& lane : track->automation)
                {
                    description << 'a' << lane.parameterId << ',' << (int) lane.points.size() << ',';

                    juce::int64 pointHash = 0;

                    for (const auto& point : lane.points)
                        pointHash = pointHash * 1000003
                                  + (juce::int64) std::llround (point.first * 4096.0) * 65537
                                  + (juce::int64) std::llround ((double) point.second * 65536.0);

                    description << pointHash << ';';
                }
            }

            for (const auto& bus : project.buses)
            {
                description << 'B' << bus.id << ';';

                for (const auto& fx : bus.builtinFx)
                    description << fx.type << ',' << (fx.bypassed ? '1' : '0') << ';';
            }

            for (const auto& fx : project.masterChain)
                description << 'M' << fx.type << ',' << (fx.bypassed ? '1' : '0') << ';';

            return description;
        }
    }

    AudioEngine::AudioEngine (Settings& settingsToUse, PluginManager& pluginManagerToUse)
        : settings (settingsToUse),
          pluginManager (pluginManagerToUse),
          mixer (std::make_unique<Mixer> (pluginManagerToUse))
    {
        formatManager.registerBasicFormats();
    }

    AudioEngine::~AudioEngine()
    {
        if (recorder.isRecording())
            recorder.finish (transport.getPositionBeats(),
                             project != nullptr ? project->tempo : TempoMap());

        deviceManager.removeAudioCallback (this);

        for (const auto& device : juce::MidiInput::getAvailableDevices())
            deviceManager.removeMidiInputDeviceCallback (device.identifier, this);

        if (project != nullptr)
            project->removeChangeListener (this);

        mixer.reset();
    }

    //==============================================================================
    void AudioEngine::setProject (Project* newProject)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (project == newProject)
            return;

        if (transport.isRecording())
            stopRecording();

        if (project != nullptr)
            project->removeChangeListener (this);

        {
            const ScopedSuspend suspend (*this);
            project = newProject;
            transport.setProject (newProject);
            transport.setPositionSamples (0);
            mixer->setProject (newProject);
            sessionClock.reset();
        }

        if (project != nullptr)
            project->addChangeListener (this);

        projectChanged();
    }

    juce::String AudioEngine::initialiseAudioDevice()
    {
        auto savedState = settings.getAudioDeviceState();
        auto error = deviceManager.initialise (2, 2, savedState.get(), true);

        if (error.isNotEmpty())         // the stored device may simply no longer be plugged in
            error = deviceManager.initialiseWithDefaultDevices (2, 2);

        if (error.isNotEmpty())
            return error;

        // The audio callback goes on first: it is what resets the MIDI collector's sample
        // rate, and a message arriving before that would have no valid timestamp.
        deviceManager.addAudioCallback (this);

        for (const auto& device : juce::MidiInput::getAvailableDevices())
        {
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
            deviceManager.addMidiInputDeviceCallback (device.identifier, this);
        }

        return {};
    }

    void AudioEngine::saveAudioDeviceState()
    {
        const auto state = deviceManager.createStateXml();
        settings.setAudioDeviceState (state.get());
        settings.flush();
    }

    double AudioEngine::getLatencyMs() const
    {
        auto* device = deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
            return 0.0;

        const auto sampleRate = device->getCurrentSampleRate();

        if (sampleRate <= 0.0)
            return 0.0;

        const auto samples = device->getInputLatencyInSamples()
                           + device->getOutputLatencyInSamples()
                           + device->getCurrentBufferSizeSamples();

        return 1000.0 * (double) samples / sampleRate;
    }

    //==============================================================================
    void AudioEngine::play()
    {
        if (project == nullptr)
        {
            reportError ("There is no project to play");
            return;
        }

        transport.play();
    }

    void AudioEngine::stop()
    {
        if (transport.isRecording() || recorder.isRecording())
        {
            stopRecording();
            return;
        }

        transport.stop();
    }

    void AudioEngine::startRecording()
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (transport.isRecording() || recorder.isRecording())
            return;

        if (project == nullptr)
        {
            reportError ("There is no project to record into");
            return;
        }

        auto* device = deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
        {
            reportError ("No audio device is open");
            return;
        }

        std::vector<Recorder::ArmedInput> armed;
        const auto mediaFolder = project->getMediaFolder();
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");

        for (int i = 0; i < project->getNumTracks(); ++i)
        {
            auto& track = project->getTrack (i);

            // ponytail: audio only.  Capturing liveMidi into a MidiClip belongs with take
            // management (spec 8.4.3) - add it there rather than bolting it on here.
            if (! track.recordArmed || track.getType() != TrackType::audio)
                continue;

            const auto name = juce::File::createLegalFileName (track.name.isNotEmpty() ? track.name
                                                                                       : juce::String ("Take"));
            armed.push_back ({ track.getId(), track.inputChannel,
                               mediaFolder.getChildFile (name + "_" + stamp + ".wav").getNonexistentSibling() });
        }

        if (armed.empty())
        {
            reportError ("No audio track is record-armed");
            return;
        }

        const auto recordBeat = transport.getPositionBeats();
        const auto result = recorder.start (armed, device->getCurrentSampleRate(),
                                            project->bitDepth, recordBeat);

        if (result.failed())
        {
            reportError (result.getErrorMessage());
            return;
        }

        // ponytail: the count-in gate is block-accurate, not sample-accurate.  Offset the
        // first write inside the block if that ever shows up as a timing complaint.
        recordFromSample.store (transport.getPositionSamples());
        metronomeBeforeCountIn = transport.isMetronomeEnabled();

        if (const auto bars = transport.getCountInBars(); bars > 0)
        {
            const auto timeSig = project->tempo.timeSignatureAt (recordBeat);
            const auto beatsPerBar = (double) timeSig.numerator * 4.0
                                        / (double) juce::jmax (1, timeSig.denominator);

            transport.setMetronomeEnabled (true);
            transport.setPositionBeats (juce::jmax (0.0, recordBeat - (double) bars * beatsPerBar));
        }

        transport.setRecording (true);
        transport.play();
    }

    void AudioEngine::stopRecording()
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (! transport.isRecording() && ! recorder.isRecording())
            return;

        const auto endBeat = transport.getPositionBeats();

        transport.setRecording (false);
        transport.stop();
        transport.setMetronomeEnabled (metronomeBeforeCountIn);

        const auto droppedBlocks = recorder.getDroppedBlockCount();
        bool addedClips = false;

        {
            // The takes are appended while the callback is parked: the mixer reads
            // track.audioClips as it plays, and a vector reallocation would be fatal.
            const ScopedSuspend suspend (*this);

            for (const auto& take : recorder.finish (endBeat, project != nullptr ? project->tempo
                                                                                  : TempoMap()))
            {
                auto* track = project != nullptr ? project->findTrack (take.trackId) : nullptr;

                if (track == nullptr)
                    continue;

                AudioClip clip;
                clip.id          = project->nextClipId();
                clip.name        = take.file.getFileNameWithoutExtension();
                clip.sourceFile  = take.file;
                clip.startBeats  = take.startBeats;
                clip.lengthBeats = take.lengthBeats;

                track->audioClips.push_back (clip);
                addedClips = true;
            }
        }

        if (droppedBlocks > 0)
            reportError (juce::String (droppedBlocks) + " block(s) were dropped while recording - "
                         "the disk could not keep up with the input");

        if (addedClips && project != nullptr)
        {
            project->markDirty();
            project->sendChangeMessage();   // AudioEngine is not a broadcaster; the document is
        }
    }

    //==============================================================================
    void AudioEngine::previewMidiClip (TrackId trackId, const MidiClip& clip)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        {
            const ScopedSuspend suspend (*this);
            previewClip = clip;             // the mixer keeps the pointer, so we own the copy
        }

        mixer->setPreviewClip (trackId, &previewClip);
    }

    void AudioEngine::stopPreview()
    {
        mixer->setPreviewClip (invalidTrackId, nullptr);
    }

    void AudioEngine::launchSessionClip (TrackId trackId, const SessionClip& clip)
    {
        if (project == nullptr)
            return;

        mixer->launchSessionClip (trackId, clip, sessionClock.nextBarBoundarySample (project->tempo));
    }

    void AudioEngine::stopSessionClip (TrackId trackId)
    {
        if (project == nullptr)
            return;

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

    float AudioEngine::getMasterPeak (int channel) const noexcept
    {
        if (channel < 0 || channel > 1)
            return 0.0f;

        return masterPeak[channel].load();
    }

    //==============================================================================
    void AudioEngine::projectChanged()
    {
        JUCE_ASSERT_MESSAGE_THREAD

        {
            const ScopedSuspend suspend (*this);
            mixer->rebuild();
        }

        topology = project != nullptr ? topologyFingerprint (*project) : juce::String();
        pushMixerState();
    }

    void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
    {
        if (project == nullptr || source != project)
            return;

        // The document broadcasts on every edit; only rebuild - which re-instantiates
        // plugins - when the shape of the graph actually changed. A clip edit (add,
        // remove, move, replace) is deliberately NOT part of that shape (see
        // topologyFingerprint's own comment), so it has to reach the mixer through
        // syncClips() instead, or playback would keep serving whatever clips were
        // in place at the last rebuild - a genuinely new track still goes through
        // the topology branch, since syncClips() only touches tracks that already
        // have a TrackState.
        if (topologyFingerprint (*project) != topology)
        {
            projectChanged();
        }
        else
        {
            {
                const ScopedSuspend suspend (*this);
                mixer->syncClips();
            }

            pushMixerState();
        }
    }

    void AudioEngine::pushMixerState()
    {
        if (project == nullptr)
            return;

        for (int i = 0; i < project->getNumTracks(); ++i)
        {
            auto& track = project->getTrack (i);
            const auto id = track.getId();

            // Solo is the mixer's own summing decision now rather than a fold into
            // mute: a soloed track still has to reach its bus, not the master.
            mixer->setSolo (id, track.soloed);
            mixer->setInputMonitoring (id, track.recordArmed, track.inputMonitoring);

            if (auto* strip = mixer->getStripForTrack (id))
            {
                strip->setGainDb (track.gainDb);
                strip->setPan (track.pan);
                strip->setMuted (track.muted);

                for (size_t p = 0; p < track.plugins.size(); ++p)
                    strip->setPluginBypassed ((int) p, track.plugins[p].bypassed);
            }
        }

        for (int i = 0; i < (int) project->buses.size(); ++i)
        {
            const auto& bus = project->buses[(size_t) i];

            if (auto* strip = mixer->getBusStrip (i))
            {
                strip->setGainDb (bus.gainDb);
                strip->setPan (bus.pan);
                strip->setMuted (bus.muted);
            }
        }
    }

    //==============================================================================
    juce::Result AudioEngine::renderToFile (const juce::File& destination, double startBeat, double endBeat,
                                            int bitDepth, double sampleRate, bool stemPerTrack,
                                            std::function<void (float)> progress)
    {
        if (project == nullptr)
            return juce::Result::fail ("There is no project to render");

        if (endBeat <= startBeat)
            return juce::Result::fail ("Nothing to render: the range is empty");

        if (sampleRate <= 0.0)
            sampleRate = project->sampleRate;

        auto* format = formatManager.findFormatForFileExtension (destination.getFileExtension());

        if (format == nullptr)
            return juce::Result::fail ("Unsupported output format: " + destination.getFileExtension());

        if (! format->getPossibleBitDepths().contains (bitDepth))
            bitDepth = 24;

        constexpr int numChannels = 2;

        struct Pass { juce::File file; TrackId trackId = invalidTrackId; };
        std::vector<Pass> passes;

        if (stemPerTrack)
        {
            for (int i = 0; i < project->getNumTracks(); ++i)
            {
                auto& track = project->getTrack (i);
                passes.push_back ({ destination.getSiblingFile (destination.getFileNameWithoutExtension()
                                                                  + "_" + juce::File::createLegalFileName (track.name)
                                                                  + destination.getFileExtension()),
                                    track.getId() });
            }
        }
        else
        {
            passes.push_back ({ destination, invalidTrackId });
        }

        if (passes.empty())
            return juce::Result::fail ("The project has no tracks to render");

        std::vector<bool> savedMutes;

        for (int i = 0; i < project->getNumTracks(); ++i)
            savedMutes.push_back (project->getTrack (i).muted);

        const auto restoreMutes = [this, &savedMutes]
        {
            for (int i = 0; i < project->getNumTracks() && i < (int) savedMutes.size(); ++i)
                project->getTrack (i).muted = savedMutes[(size_t) i];
        };

        // The live callback must not run the mixer while it is prepared for the offline
        // rate; the caller may also have stopped the device altogether.
        const ScopedSuspend suspend (*this);

        const juce::ScopeGuard restoreLiveState { [this, &restoreMutes]
        {
            restoreMutes();
            pushMixerState();

            if (auto* device = deviceManager.getCurrentAudioDevice())
                mixer->prepare (device->getCurrentSampleRate(),
                                device->getCurrentBufferSizeSamples(),
                                juce::jmax (2, device->getActiveOutputChannels().countNumberOfSetBits()));
        } };

        mixer->prepare (sampleRate, offlineBlockSize, numChannels);

        const auto startSample  = (juce::int64) std::llround (project->tempo.beatsToSeconds (startBeat) * sampleRate);
        const auto endSample    = (juce::int64) std::llround (project->tempo.beatsToSeconds (endBeat)   * sampleRate);
        const auto totalSamples = juce::jmax ((juce::int64) 1,
                                              (endSample - startSample) * (juce::int64) passes.size());
        juce::int64 samplesDone = 0;

        juce::AudioBuffer<float> buffer (numChannels, offlineBlockSize);
        juce::MidiBuffer noLiveMidi;
        const juce::AudioBuffer<float> noDeviceInput;   // offline: nothing to monitor

        for (const auto& pass : passes)
        {
            // ponytail: stems are one muted-solo pass per track over the whole range.
            // A per-strip output tap in Mixer would do it in a single pass.
            if (pass.trackId != invalidTrackId)
            {
                for (int i = 0; i < project->getNumTracks(); ++i)
                {
                    auto& track = project->getTrack (i);
                    track.muted = track.getId() != pass.trackId;
                }
            }
            else
            {
                restoreMutes();
            }

            pushMixerState();

            pass.file.getParentDirectory().createDirectory();
            pass.file.deleteFile();

            std::unique_ptr<juce::FileOutputStream> stream (pass.file.createOutputStream());

            if (stream == nullptr || ! stream->openedOk())
                return juce::Result::fail ("Could not write to " + pass.file.getFullPathName());

            std::unique_ptr<juce::AudioFormatWriter> writer (
                format->createWriterFor (stream.get(), sampleRate, (unsigned int) numChannels, bitDepth, {}, 0));

            if (writer == nullptr)
                return juce::Result::fail ("Could not create a writer for " + pass.file.getFullPathName());

            [[maybe_unused]] const auto* rawStream = stream.release();               // the writer owns the stream from here

            for (auto position = startSample; position < endSample; position += offlineBlockSize)
            {
                const auto numSamples = (int) juce::jmin ((juce::int64) offlineBlockSize, endSample - position);

                buffer.clear();
                juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), numChannels, numSamples);
                noLiveMidi.clear();

                // -1: explicitly no SessionClock position for this call (see
                // Mixer::process's doc comment) - an offline render must never
                // adopt/replace/advance/render a live Session view clip, whatever
                // might be looping on some track at the moment export is clicked.
                // Not relying on the default argument to silently do this: the
                // parameter used to default to 0, a valid-looking real position
                // that made this exact call corrupt both the export and the
                // live session clip's playback state.
                mixer->process (block, position, noLiveMidi, noDeviceInput, true, -1);

                if (! writer->writeFromAudioSampleBuffer (block, 0, numSamples))
                    return juce::Result::fail ("Ran out of disk space writing " + pass.file.getFullPathName());

                samplesDone += numSamples;

                if (progress != nullptr)
                    progress (juce::jlimit (0.0f, 1.0f, (float) ((double) samplesDone / (double) totalSamples)));
            }
        }

        if (progress != nullptr)
            progress (1.0f);

        return juce::Result::ok();
    }

    //==============================================================================
    void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                        float* const* outputChannelData, int numOutputChannels,
                                                        int numSamples,
                                                        const juce::AudioIODeviceCallbackContext&)
    {
        insideCallback.store (true);

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[channel], numSamples);

        // Suspended, or the driver handed us a bigger block than it promised: stay silent.
        if (suspended.load() || numSamples <= 0
             || renderBuffer.getNumChannels() <= 0 || numSamples > renderBuffer.getNumSamples())
        {
            insideCallback.store (false);
            return;
        }

        const auto blockStart = transport.advance (numSamples);

        // Captured BEFORE advance(), exactly like transport.advance() itself
        // returns ITS pre-advance position - this is SessionClock's own
        // free-running timeline, deliberately not `blockStart` (Transport's),
        // since Transport freezes while stopped and can jump/go backwards on a
        // seek or loop.  mixer->process() below compares every Session view
        // launch/stop boundary against this value, never against `blockStart`.
        const auto sessionBlockStart = sessionClock.currentSample();
        sessionClock.advance (numSamples);   // free-running: never gated on play/stop

        const auto numRenderChannels = juce::jmin (renderBuffer.getNumChannels(),
                                                   juce::jmax (numOutputChannels, 1));

        juce::AudioBuffer<float> output (renderBuffer.getArrayOfWritePointers(), numRenderChannels, numSamples);
        output.clear();

        liveMidi.clear();
        midiCollector.removeNextBlockOfMessages (liveMidi, numSamples);

        /*  The driver's input pointers are copied once, up front: input monitoring
            and the recorder both want this block, and neither may hold on to the
            driver's own memory.                                                    */
        const auto numInputsCopied = numSamples <= inputBuffer.getNumSamples()
                                       ? juce::jmax (0, juce::jmin (inputBuffer.getNumChannels(),
                                                                    numInputChannels))
                                       : 0;

        for (int channel = 0; channel < numInputsCopied; ++channel)
        {
            if (inputChannelData[channel] != nullptr)
                inputBuffer.copyFrom (channel, 0, inputChannelData[channel], numSamples);
            else
                inputBuffer.clear (channel, 0, numSamples);
        }

        juce::AudioBuffer<float> inputs (inputBuffer.getArrayOfWritePointers(),
                                         numInputsCopied, numSamples);

        mixer->process (output, blockStart, liveMidi, inputs, transport.isPlaying(), sessionBlockStart);

        if (transport.isMetronomeEnabled() && transport.isPlaying())
            renderMetronome (output, blockStart, numSamples);

        if (recorder.isRecording()
             && blockStart + numSamples > recordFromSample.load()
             && numInputsCopied > 0)
            recorder.processBlock (inputs, numSamples);

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto blockPeak = channel < numRenderChannels ? output.getMagnitude (channel, 0, numSamples)
                                                               : 0.0f;
            masterPeak[channel].store (juce::jmax (blockPeak, masterPeak[channel].load() * meterDecayPerBlock));
        }

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[channel],
                                                   output.getReadPointer (juce::jmin (channel, numRenderChannels - 1)),
                                                   numSamples);

        insideCallback.store (false);
    }

    void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
    {
        const auto sampleRate = device->getCurrentSampleRate();
        const auto blockSize  = juce::jmax (64, device->getCurrentBufferSizeSamples());
        const auto numOutputs = juce::jmax (2, device->getActiveOutputChannels().countNumberOfSetBits());
        const auto numInputs  = juce::jmax (1, device->getActiveInputChannels().countNumberOfSetBits());

        transport.setSampleRate (sampleRate);
        sessionClock.prepare (sampleRate);
        midiCollector.reset (sampleRate);

        renderBuffer.setSize (numOutputs, blockSize, false, true, true);
        inputBuffer .setSize (numInputs,  blockSize, false, true, true);

        clickPhase = 0.0;
        clickLevel = 0.0f;
        clickDecay = (float) std::exp (-1.0 / (0.025 * juce::jmax (8000.0, sampleRate)));

        mixer->prepare (sampleRate, blockSize, numOutputs);
    }

    void AudioEngine::audioDeviceStopped()
    {
        mixer->releaseResources();

        for (auto& peak : masterPeak)
            peak.store (0.0f);

        clickLevel = 0.0f;
    }

    void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
    {
        midiCollector.addMessageToQueue (message);
    }

    void AudioEngine::renderMetronome (juce::AudioBuffer<float>& output, juce::int64 blockStart,
                                       int numSamples) noexcept
    {
        auto* p = transport.getProject();
        const auto sampleRate = transport.getSampleRate();

        if (p == nullptr || sampleRate <= 0.0)
            return;

        const auto startBeats = p->tempo.secondsToBeats ((double) blockStart / sampleRate);
        const auto endBeats   = p->tempo.secondsToBeats ((double) (blockStart + numSamples) / sampleRate);

        int renderFrom = 0;

        // One click per block: a block only spans a whole beat at absurd tempi.
        if (const auto beat = std::ceil (startBeats - 1.0e-9); beat < endBeats)
        {
            const auto trigger = (juce::int64) std::llround (p->tempo.beatsToSeconds (beat) * sampleRate)
                                    - blockStart;
            renderFrom = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1, trigger);

            int bar = 1;
            double beatInBar = 0.0;
            p->tempo.barAndBeat (beat, bar, beatInBar);

            clickPhase = 0.0;
            clickPhaseDelta = juce::MathConstants<double>::twoPi * (beatInBar < 0.01 ? 1600.0 : 1000.0) / sampleRate;
            clickLevel = 0.4f;
        }

        for (int i = renderFrom; i < numSamples && clickLevel > 1.0e-4f; ++i)
        {
            const auto sample = (float) std::sin (clickPhase) * clickLevel;

            for (int channel = 0; channel < output.getNumChannels(); ++channel)
                output.addSample (channel, i, sample);

            clickPhase += clickPhaseDelta;
            clickLevel *= clickDecay;
        }
    }

    //==============================================================================
    void AudioEngine::suspendAudio()
    {
        suspendLock.enter();               // juce::CriticalSection is re-entrant on the same thread

        if (++suspendCount == 1)
        {
            suspended.store (true);

            // Sequentially consistent: the callback raises insideCallback before it reads
            // `suspended`, so once we see it low the next block is guaranteed to bail out.
            while (insideCallback.load())
                juce::Thread::sleep (1);
        }
    }

    void AudioEngine::resumeAudio()
    {
        if (--suspendCount == 0)
            suspended.store (false);

        suspendLock.exit();
    }

    void AudioEngine::reportError (const juce::String& message)
    {
        if (! onError)
            return;

        if (juce::MessageManager::existsAndIsCurrentThread())
            onError (message);
        else
            juce::MessageManager::callAsync ([callback = onError, message] { callback (message); });
    }
}
