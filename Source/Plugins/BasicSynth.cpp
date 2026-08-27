#include "Plugins/BasicSynth.h"
#include <cmath>

namespace ss
{

namespace
{
    constexpr int numVoices = 16;
}

//==============================================================================
struct BasicSynth::Params
{
    // Read every sample by every rendering voice, written from setValue() -
    // which Mixer.cpp's applyAutomation() calls from the audio thread for
    // automated parameters. Plain atomics: same thread on both ends in
    // practice, this just avoids a torn read if that ever changes.
    std::atomic<int>   waveform { 0 };   // 0 sine, 1 saw, 2 square
    std::atomic<float> gain     { 0.7f };
    std::atomic<float> attack   { 0.01f };
    std::atomic<float> release  { 0.25f };
};

//==============================================================================
namespace
{
    /** setValue()/getValue() on an AudioProcessorParameter are always 0..1
        normalised - NormalisableRange does the real<->normalised conversion so
        each parameter below only has to say what it means, not how to scale it. */
    class FloatParam final : public juce::AudioPluginInstance::HostedParameter
    {
    public:
        FloatParam (std::atomic<float>& target, juce::NormalisableRange<float> r,
                   juce::String nm, juce::String lbl)
            : value (target), range (r), paramName (std::move (nm)), paramLabel (std::move (lbl))
        {
        }

        juce::String getParameterID() const override { return paramName; }
        float getValue() const override               { return range.convertTo0to1 (value.load()); }
        void setValue (float v) override               { value.store (range.convertFrom0to1 (v)); }
        float getDefaultValue() const override         { return range.convertTo0to1 (value.load()); }
        juce::String getName (int n) const override    { return paramName.substring (0, n); }
        juce::String getLabel() const override         { return paramLabel; }
        int getNumSteps() const override               { return juce::AudioProcessor::getDefaultNumParameterSteps(); }
        bool isDiscrete() const override               { return false; }
        float getValueForText (const juce::String& text) const override
        {
            return range.convertTo0to1 (text.getFloatValue());
        }

    private:
        std::atomic<float>& value;
        juce::NormalisableRange<float> range;
        juce::String paramName, paramLabel;
    };

    /** Waveform choice - 3 discrete steps (sine/saw/square). */
    class WaveformParam final : public juce::AudioPluginInstance::HostedParameter
    {
    public:
        explicit WaveformParam (std::atomic<int>& target) : value (target) {}

        juce::String getParameterID() const override { return "waveform"; }
        float getValue() const override               { return (float) value.load() / 2.0f; }
        void setValue (float v) override               { value.store (juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 2.0f)); }
        float getDefaultValue() const override         { return 0.0f; }
        juce::String getName (int n) const override    { return juce::String ("Waveform").substring (0, n); }
        juce::String getLabel() const override         { return {}; }
        int getNumSteps() const override               { return 3; }
        bool isDiscrete() const override               { return true; }
        juce::String getText (float v, int) const override
        {
            switch (juce::roundToInt (v * 2.0f)) { case 1: return "Saw"; case 2: return "Square"; default: return "Sine"; }
        }
        float getValueForText (const juce::String& text) const override
        {
            if (text == "Saw") return 0.5f;
            if (text == "Square") return 1.0f;
            return 0.0f;
        }

    private:
        std::atomic<int>& value;
    };
}

//==============================================================================
class BasicSynth::Sound final : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

//==============================================================================
class BasicSynth::Voice final : public juce::SynthesiserVoice
{
public:
    explicit Voice (const Params& p) : params (p) {}

    bool canPlaySound (juce::SynthesiserSound*) override { return true; }

    void startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound*, int) override
    {
        frequency = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        phase = 0.0;
        level = velocity;

        juce::ADSR::Parameters adsr;
        adsr.attack  = juce::jmax (0.001f, params.attack.load());
        adsr.decay   = 0.05f;
        adsr.sustain = 1.0f;
        adsr.release = juce::jmax (0.01f, params.release.load());
        envelope.setParameters (adsr);
        envelope.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
            envelope.noteOff();
        else
        {
            envelope.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void setCurrentPlaybackSampleRate (double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);
        if (newRate > 0.0)
            envelope.setSampleRate (newRate);
    }

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
    {
        if (! isVoiceActive())
            return;

        const auto sr = getSampleRate();
        if (sr <= 0.0)
            return;

        const auto phaseInc = juce::MathConstants<double>::twoPi * frequency / sr;
        const auto wf = params.waveform.load();
        const auto g  = params.gain.load() * level;

        for (int i = 0; i < numSamples; ++i)
        {
            float sample;

            switch (wf)
            {
                case 1:  sample = (float) (phase / juce::MathConstants<double>::pi - 1.0); break;      // saw: 0..2pi -> -1..1
                case 2:  sample = std::sin (phase) >= 0.0 ? 1.0f : -1.0f;                  break;      // square
                default: sample = (float) std::sin (phase);                               break;      // sine
            }

            sample *= g * envelope.getNextSample();

            for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
                outputBuffer.addSample (ch, startSample + i, sample);

            phase += phaseInc;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }

        if (! envelope.isActive())
            clearCurrentNote();
    }

private:
    const Params& params;
    double frequency = 440.0, phase = 0.0;
    float level = 1.0f;
    juce::ADSR envelope;
};

//==============================================================================
BasicSynth::BasicSynth()
    : AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (std::make_unique<Params>())
{
    for (int i = 0; i < numVoices; ++i)
        synth.addVoice (new Voice (*params));

    synth.addSound (new Sound());

    addHostedParameter (std::make_unique<WaveformParam> (params->waveform));
    addHostedParameter (std::make_unique<FloatParam> (params->gain,
        juce::NormalisableRange<float> (0.0f, 1.0f), "gain", ""));
    addHostedParameter (std::make_unique<FloatParam> (params->attack,
        juce::NormalisableRange<float> (0.001f, 2.0f, 0.0f, 0.4f), "attack", "s"));
    addHostedParameter (std::make_unique<FloatParam> (params->release,
        juce::NormalisableRange<float> (0.01f, 4.0f, 0.0f, 0.4f), "release", "s"));
}

BasicSynth::~BasicSynth() = default;

void BasicSynth::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
    juce::ignoreUnused (samplesPerBlock);
}

void BasicSynth::releaseResources() {}

void BasicSynth::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());
}

void BasicSynth::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out (destData, false);
    out.writeInt (params->waveform.load());
    out.writeFloat (params->gain.load());
    out.writeFloat (params->attack.load());
    out.writeFloat (params->release.load());
}

void BasicSynth::setStateInformation (const void* data, int sizeInBytes)
{
    if (sizeInBytes < (int) (sizeof (juce::int32) + 3 * sizeof (float)))
        return; // nothing/garbage saved - keep the defaults rather than half-apply

    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    params->waveform.store (in.readInt());
    params->gain.store (in.readFloat());
    params->attack.store (in.readFloat());
    params->release.store (in.readFloat());
}

void BasicSynth::fillInPluginDescription (juce::PluginDescription& d) const
{
    d = juce::PluginDescription();
    d.name = getName();
    d.pluginFormatName = "KANADE DAW";
    d.fileOrIdentifier = identifier;
    d.isInstrument = true;
    d.numInputChannels = 0;
    d.numOutputChannels = 2;
}

//==============================================================================
class BasicSynthUnitTests final : public juce::UnitTest
{
public:
    BasicSynthUnitTests() : juce::UnitTest ("ScoreSmith basic synth", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("a held note produces non-silent output");
        {
            BasicSynth synth;
            synth.prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 69, 1.0f), 0); // A4

            for (int i = 0; i < 10; ++i)
            {
                synth.processBlock (buffer, midi);
                midi.clear();
            }

            expect (buffer.getMagnitude (0, 0, buffer.getNumSamples()) > 0.01f,
                    "a held note should be audible after the attack ramps up");
        }

        beginTest ("note-off releases to silence");
        {
            BasicSynth synth;
            synth.prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 69, 1.0f), 0);
            synth.processBlock (buffer, midi);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
            synth.processBlock (buffer, midi);
            midi.clear();

            // The release tail is short (default 0.25s); well past it, the
            // voice must have stopped rather than looping forever.
            for (int i = 0; i < 200; ++i)
                synth.processBlock (buffer, midi);

            expect (buffer.getMagnitude (0, 0, buffer.getNumSamples()) < 1.0e-4f,
                    "a released note should decay to silence, not sustain forever");
        }

        beginTest ("parameters and state round-trip");
        {
            BasicSynth synth;
            expectEquals (synth.getParameters().size(), 4);

            for (auto* p : synth.getParameters())
                p->setValue (0.75f);

            juce::MemoryBlock state;
            synth.getStateInformation (state);

            BasicSynth reloaded;
            reloaded.setStateInformation (state.getData(), (int) state.getSize());

            for (int i = 0; i < synth.getParameters().size(); ++i)
                expectWithinAbsoluteError (reloaded.getParameters()[i]->getValue(),
                                           synth.getParameters()[i]->getValue(), 1.0e-3f,
                                           "parameter " + juce::String (i) + " did not survive a state round trip");
        }

        beginTest ("PluginManager routes the builtin identifier without a real plugin lookup");
        {
            expectEquals (juce::String (BasicSynth::identifier), juce::String ("builtin:basicsynth"));
        }
    }
};

static BasicSynthUnitTests basicSynthUnitTests;

}
