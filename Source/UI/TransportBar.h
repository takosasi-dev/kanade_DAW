#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    /** The bottom strip of the main window (spec 9.1 / 9.5):
        transport buttons, bar:beat + timecode readout, tempo and time signature,
        metronome and count-in, master meter and CPU meter. */
    class TransportBar final : public ProjectView,
                               private juce::Timer
    {
    public:
        TransportBar (AppContext&, UiState&, juce::ApplicationCommandManager&);
        ~TransportBar() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

    private:
        void timerCallback() override;
        void pushTempoToProject();
        void pushTimeSignatureToProject();
        void pullFromProject();

        juce::ApplicationCommandManager& commands;

        juce::TextButton returnToStartButton, playButton, stopButton, recordButton, loopButton;
        juce::TextButton metronomeButton;
        juce::ComboBox   countInBox;
        juce::Slider     tempoSlider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
        juce::ComboBox   timeSigNumerator, timeSigDenominator;
        juce::Label      barBeatLabel, timecodeLabel, cpuLabel;

        juce::Rectangle<float> meterArea;
        float meterPeak[2] { 0.0f, 0.0f };
        bool  updatingFromProject = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
    };
}
