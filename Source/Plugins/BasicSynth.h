#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace ss
{
    /** A minimal always-available polyphonic synth (sine/saw/square + ADSR), so a
        MIDI track makes SOME sound without needing an external VST instrument -
        this machine, like most fresh installs, has none. Selected from the mixer's
        "Add instrument" menu (MixerView.cpp) exactly like a real plugin, and
        constructed by PluginManager::createInstance() for the special identifier
        "builtin:basicsynth" - it never goes through plugin scanning, sandboxing or
        the host-process proxy, since it's ScoreSmith's own trusted code.

        ponytail: one oscillator per voice, no filter/LFO/detune. Ceiling is "a
        usable placeholder voice", not a real instrument - upgrade path is the
        still-missing sf2/SFZ sample playback (docs/STATUS.md P0 gap), which this
        does not attempt to replace. */
    class BasicSynth final : public juce::AudioPluginInstance
    {
    public:
        BasicSynth();
        ~BasicSynth() override;

        static constexpr const char* identifier = "builtin:basicsynth";

        const juce::String getName() const override { return "KANADE DAW Basic Synth"; }
        void prepareToPlay (double sampleRate, int samplesPerBlock) override;
        void releaseResources() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        bool acceptsMidi() const override  { return true; }
        bool producesMidi() const override { return false; }
        double getTailLengthSeconds() const override { return 1.0; }

        bool hasEditor() const override                     { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }

        void getStateInformation (juce::MemoryBlock&) override;
        void setStateInformation (const void*, int) override;

        int getNumPrograms() override                              { return 1; }
        int getCurrentProgram() override                           { return 0; }
        void setCurrentProgram (int) override                      {}
        const juce::String getProgramName (int) override           { return {}; }
        void changeProgramName (int, const juce::String&) override {}

        void fillInPluginDescription (juce::PluginDescription& d) const override;

    private:
        class Sound;
        class Voice;
        struct Params;

        juce::Synthesiser synth;
        std::unique_ptr<Params> params;
    };
}
