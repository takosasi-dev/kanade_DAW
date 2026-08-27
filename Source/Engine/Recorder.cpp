#include "Engine/Recorder.h"

namespace ss
{
    namespace
    {
        // ~1.4 s of headroom per channel at 48 kHz before a slow disk starts costing blocks.
        constexpr int fifoSamplesPerChannel = 1 << 16;
    }

    struct Recorder::Channel
    {
        TrackId    trackId = invalidTrackId;
        int        hardwareChannel = 0;
        juce::File file;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        std::atomic<float> peak { 0.0f };
        juce::int64 samplesWritten = 0;     // audio thread only
    };

    Recorder::Recorder() = default;

    Recorder::~Recorder()
    {
        active.store (false);
        channels.clear();                  // flushes and closes any writer still open
        writerThread.stopThread (2000);
    }

    juce::Result Recorder::start (const std::vector<ArmedInput>& inputs, double sampleRate,
                                  int bitDepth, double startBeat)
    {
        if (active.load())
            return juce::Result::fail ("A recording is already running");

        if (inputs.empty())
            return juce::Result::fail ("No armed inputs");

        if (sampleRate <= 0.0)
            return juce::Result::fail ("The audio device is not running");

        channels.clear();
        dropped.store (0);
        startBeatOfTake = startBeat;
        sampleRateOfTake = sampleRate;

        juce::WavAudioFormat wav;

        // WavAudioFormatWriter switches to IEEE float on its own at 32 bits.
        if (! wav.getPossibleBitDepths().contains (bitDepth))
            bitDepth = 24;

        if (! writerThread.isThreadRunning())
            writerThread.startThread();

        for (const auto& input : inputs)
        {
            auto channel = std::make_unique<Channel>();
            channel->trackId         = input.trackId;
            channel->hardwareChannel = input.hardwareChannel;
            channel->file            = input.destination;

            channel->file.getParentDirectory().createDirectory();
            channel->file.deleteFile();

            std::unique_ptr<juce::FileOutputStream> stream (channel->file.createOutputStream());

            if (stream == nullptr || ! stream->openedOk())
            {
                channels.clear();
                return juce::Result::fail ("Could not create " + channel->file.getFullPathName());
            }

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream.get(), sampleRate, 1u, bitDepth, {}, 0));

            if (writer == nullptr)
            {
                channels.clear();
                return juce::Result::fail ("Cannot write " + juce::String (bitDepth)
                                           + "-bit WAV to " + channel->file.getFullPathName());
            }

            [[maybe_unused]] const auto* rawStream = stream.release();              // the writer owns the stream from here

            channel->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
                                  writer.release(), writerThread, fifoSamplesPerChannel);

            channels.push_back (std::move (channel));
        }

        active.store (true);
        return juce::Result::ok();
    }

    void Recorder::processBlock (const juce::AudioBuffer<float>& inputs, int numSamples) noexcept
    {
        if (numSamples <= 0 || ! active.load())
            return;

        const auto numInputChannels = inputs.getNumChannels();

        for (auto& channelPtr : channels)
        {
            auto& channel = *channelPtr;

            if (channel.hardwareChannel < 0 || channel.hardwareChannel >= numInputChannels)
            {
                channel.peak.store (0.0f);
                continue;
            }

            channel.peak.store (inputs.getMagnitude (channel.hardwareChannel, 0, numSamples));

            const float* const source[] = { inputs.getReadPointer (channel.hardwareChannel) };

            if (channel.writer->write (source, numSamples))
                channel.samplesWritten += numSamples;
            else
                dropped.fetch_add (1);     // FIFO full: lose the block rather than the stream
        }
    }

    std::vector<Recorder::Take> Recorder::finish (double endBeat, const TempoMap& tempo)
    {
        std::vector<Take> takes;

        if (! active.exchange (false))
            return takes;

        const auto startSeconds = tempo.beatsToSeconds (startBeatOfTake);

        // Only reached when start() never saw a valid device rate.
        const auto fallbackLength = juce::jmax (0.0, endBeat - startBeatOfTake);

        for (auto& channelPtr : channels)
        {
            auto& channel = *channelPtr;

            channel.writer.reset();        // flushes the FIFO and closes the file
            channel.peak.store (0.0f);

            if (channel.samplesWritten <= 0)
            {
                channel.file.deleteFile();
                continue;
            }

            /*  Each take is as long as what actually reached the disk, not as long
                as the transport ran: a channel that dropped blocks writes a shorter
                file, and a clip claiming the full range would show silence that is
                not in the take.                                                    */
            const auto lengthBeats = sampleRateOfTake > 0.0
                ? juce::jmax (0.0, tempo.secondsToBeats (startSeconds + (double) channel.samplesWritten
                                                                          / sampleRateOfTake)
                                       - startBeatOfTake)
                : fallbackLength;

            takes.push_back ({ channel.trackId, channel.file, startBeatOfTake, lengthBeats });
        }

        channels.clear();
        writerThread.stopThread (2000);
        return takes;
    }

    float Recorder::getInputPeak (int armedIndex) const noexcept
    {
        if (armedIndex < 0 || armedIndex >= (int) channels.size())
            return 0.0f;

        return channels[(size_t) armedIndex]->peak.load();
    }
}
