#include "UI/TransportBar.h"
#include "Engine/AudioEngine.h"
#include <cmath>

namespace ss
{
    TransportBar::TransportBar (AppContext& c, UiState& s, juce::ApplicationCommandManager& cm)
        : ProjectView (c, s), commands (cm)
    {
        auto addCommandButton = [this] (juce::TextButton& b, const juce::String& text,
                                        juce::CommandID id, juce::Colour on)
        {
            b.setButtonText (text);
            b.setCommandToTrigger (&commands, id, true);
            b.setColour (juce::TextButton::buttonOnColourId, on);
            b.setClickingTogglesState (false);
            addAndMakeVisible (b);
        };

        const auto& p = palette();
        addCommandButton (returnToStartButton, juce::String (juce::CharPointer_UTF8 ("\xe2\x8f\xae")),
                          CommandIDs::transportReturnToStart, p.accentDim);
        addCommandButton (playButton, juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6")),
                          CommandIDs::transportPlay, p.success);
        addCommandButton (stopButton, juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa0")),
                          CommandIDs::transportStop, p.accentDim);
        addCommandButton (recordButton, juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f")),
                          CommandIDs::transportRecord, p.recordArm);
        addCommandButton (loopButton, TRANS ("Loop"), CommandIDs::transportLoop, p.accentDim);
        addCommandButton (metronomeButton, TRANS ("Click"), CommandIDs::toggleMetronome, p.accentDim);

        playButton.setTooltip (TRANS ("Play/Pause"));
        recordButton.setTooltip (TRANS ("Record"));
        recordButton.setColour (juce::TextButton::textColourOffId, p.recordArm);

        countInBox.addItem (TRANS ("No count-in"), 1);
        countInBox.addItem (TRANS ("1 bar count-in"), 2);
        countInBox.addItem (TRANS ("2 bar count-in"), 3);
        countInBox.setSelectedId (1, juce::dontSendNotification);
        countInBox.setTooltip (TRANS ("Count-in before recording"));
        countInBox.onChange = [this]
        {
            if (ctx.engine != nullptr)
                ctx.engine->getTransport().setCountInBars (countInBox.getSelectedId() - 1);
        };
        addAndMakeVisible (countInBox);

        tempoSlider.setRange (20.0, 300.0, 0.01);
        tempoSlider.setValue (120.0, juce::dontSendNotification);
        tempoSlider.setNumDecimalPlacesToDisplay (2);
        tempoSlider.setTextValueSuffix (" BPM");
        tempoSlider.setTooltip (TRANS ("Project tempo"));
        // Commit on release / on typed entry only: mutating the tempo map during
        // the drag would poison the undo snapshot taken when the drag ends.
        tempoSlider.onDragEnd     = [this] { pushTempoToProject(); };
        tempoSlider.onValueChange = [this] { if (! tempoSlider.isMouseButtonDown()) pushTempoToProject(); };
        addAndMakeVisible (tempoSlider);

        for (int n = 1; n <= 16; ++n)
            timeSigNumerator.addItem (juce::String (n), n);
        for (int d : { 1, 2, 4, 8, 16 })
            timeSigDenominator.addItem (juce::String (d), d);
        timeSigNumerator.setSelectedId (4, juce::dontSendNotification);
        timeSigDenominator.setSelectedId (4, juce::dontSendNotification);
        timeSigNumerator.onChange   = [this] { pushTimeSignatureToProject(); };
        timeSigDenominator.onChange = [this] { pushTimeSignatureToProject(); };
        timeSigNumerator.setTooltip (TRANS ("Time signature"));
        addAndMakeVisible (timeSigNumerator);
        addAndMakeVisible (timeSigDenominator);

        auto setUpReadout = [this] (juce::Label& l, float size, juce::Colour colour)
        {
            l.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size,
                                                      juce::Font::plain)));
            l.setColour (juce::Label::textColourId, colour);
            l.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (l);
        };
        setUpReadout (barBeatLabel, 21.0f, palette().textBright);
        setUpReadout (timecodeLabel, 13.0f, palette().textDim);
        setUpReadout (cpuLabel, 11.0f, palette().textDim);

        outputDeviceBox.setTooltip (TRANS ("Output device"));
        outputDeviceBox.onChange = [this]
        {
            if (ctx.engine == nullptr)
                return;

            auto& dm = ctx.engine->getDeviceManager();
            auto setup = dm.getAudioDeviceSetup();
            setup.outputDeviceName = outputDeviceBox.getText();
            dm.setAudioDeviceSetup (setup, true);
        };
        addAndMakeVisible (outputDeviceBox);
        refreshOutputDeviceBox();

        if (ctx.engine != nullptr)
            ctx.engine->getDeviceManager().addChangeListener (this);

        pullFromProject();
        startTimerHz (24);
    }

    TransportBar::~TransportBar()
    {
        stopTimer();
        if (ctx.engine != nullptr)
            ctx.engine->getDeviceManager().removeChangeListener (this);
    }

    void TransportBar::changeListenerCallback (juce::ChangeBroadcaster* source)
    {
        if (ctx.engine != nullptr && source == &ctx.engine->getDeviceManager())
            refreshOutputDeviceBox();
        else
            pullFromProject();
    }

    void TransportBar::refreshOutputDeviceBox()
    {
        if (ctx.engine == nullptr)
            return;

        auto& dm = ctx.engine->getDeviceManager();
        auto* type = dm.getCurrentDeviceTypeObject();
        if (type == nullptr)
            return;

        type->scanForDevices();
        const auto names = type->getDeviceNames (false);   // false = output device names

        outputDeviceBox.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i)
            outputDeviceBox.addItem (names[i], i + 1);

        int selectedId = 0;
        if (auto* device = dm.getCurrentAudioDevice())
            selectedId = names.indexOf (device->getName()) + 1;
        outputDeviceBox.setSelectedId (selectedId, juce::dontSendNotification);
    }

    void TransportBar::pullFromProject()
    {
        if (ctx.project == nullptr)
            return;

        const juce::ScopedValueSetter<bool> guard (updatingFromProject, true);

        const auto& events = project().tempo.getEvents();
        tempoSlider.setValue (events.empty() ? 120.0 : events.front().bpm, juce::dontSendNotification);

        const auto ts = project().tempo.timeSignatureAt (0.0);
        timeSigNumerator.setSelectedId (juce::jlimit (1, 16, ts.numerator), juce::dontSendNotification);
        timeSigDenominator.setSelectedId (ts.denominator, juce::dontSendNotification);

        loopButton.setToggleState (project().loopEnabled, juce::dontSendNotification);
    }

    void TransportBar::pushTempoToProject()
    {
        if (updatingFromProject || ctx.project == nullptr)
            return;

        const double bpm = tempoSlider.getValue();
        const auto& existing = project().tempo.getEvents();
        if (! existing.empty() && std::abs (existing.front().bpm - bpm) < 1.0e-4)
            return;   // no change, no undo entry

        performProjectEdit (project(), TRANS ("Change tempo"), [this, bpm]
        {
            auto events = project().tempo.getEvents();
            if (events.empty()) events.push_back ({ 0.0, bpm });
            else                events.front().bpm = bpm;
            project().tempo.setEvents (std::move (events));
        });
    }

    void TransportBar::pushTimeSignatureToProject()
    {
        if (updatingFromProject || ctx.project == nullptr)
            return;

        const int num = juce::jmax (1, timeSigNumerator.getSelectedId());
        const int den = juce::jmax (1, timeSigDenominator.getSelectedId());

        const auto existing = project().tempo.timeSignatureAt (0.0);
        if (existing.numerator == num && existing.denominator == den)
            return;

        performProjectEdit (project(), TRANS ("Change time signature"), [this, num, den]
        {
            auto sigs = project().tempo.getTimeSignatures();
            if (sigs.empty()) sigs.push_back ({ 0.0, num, den });
            else            { sigs.front().numerator = num; sigs.front().denominator = den; }
            project().tempo.setTimeSignatures (std::move (sigs));
        });
    }

    void TransportBar::timerCallback()
    {
        if (ctx.project == nullptr || ctx.engine == nullptr)
            return;

        auto& transport = ctx.engine->getTransport();
        const double beats = transport.getPositionBeats();

        barBeatLabel.setText (formatBarBeat (project().tempo, beats), juce::dontSendNotification);
        timecodeLabel.setText (formatTimecode (project().tempo, beats), juce::dontSendNotification);

        const double cpu = ctx.engine->getDeviceManager().getCpuUsage() * 100.0;
        cpuLabel.setText ("CPU " + juce::String (cpu, 1) + "%", juce::dontSendNotification);
        cpuLabel.setColour (juce::Label::textColourId,
                            cpu > 80.0 ? palette().danger
                                       : cpu > 55.0 ? palette().warning : palette().textDim);

        for (int ch = 0; ch < 2; ++ch)
        {
            const float level = ctx.engine->getMasterPeak (ch);
            // Fast attack, slow release so a transient stays visible for a frame or two.
            meterPeak[ch] = level > meterPeak[ch] ? level : meterPeak[ch] * 0.82f;
        }

        repaint (meterArea.getSmallestIntegerContainer().expanded (1));
    }

    void TransportBar::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.panelBg);
        g.setColour (p.divider);
        g.drawHorizontalLine (0, 0.0f, (float) getWidth());

        if (! meterArea.isEmpty())
        {
            auto left  = meterArea.withHeight (meterArea.getHeight() * 0.5f).reduced (0.0f, 1.0f);
            auto right = left.withY (meterArea.getCentreY() + 1.0f);
            paintMeter (g, left,  meterPeak[0], -1.0f, false);
            paintMeter (g, right, meterPeak[1], -1.0f, false);
        }
    }

    void TransportBar::resized()
    {
        auto area = getLocalBounds().reduced (6, 5);

        auto transportRow = area.removeFromLeft (250);
        auto buttonFor = [&transportRow] (juce::Component& c, int w)
        {
            c.setBounds (transportRow.removeFromLeft (w).reduced (2, 0));
        };
        buttonFor (returnToStartButton, 38);
        buttonFor (playButton, 46);
        buttonFor (stopButton, 38);
        buttonFor (recordButton, 42);
        buttonFor (loopButton, 52);

        area.removeFromLeft (10);
        auto readout = area.removeFromLeft (200);
        barBeatLabel.setBounds (readout.removeFromTop (readout.getHeight() * 2 / 3));
        timecodeLabel.setBounds (readout);

        area.removeFromLeft (10);
        tempoSlider.setBounds (area.removeFromLeft (150).reduced (0, 6));
        area.removeFromLeft (8);
        timeSigNumerator.setBounds (area.removeFromLeft (52).reduced (0, 6));
        area.removeFromLeft (2);
        timeSigDenominator.setBounds (area.removeFromLeft (52).reduced (0, 6));

        area.removeFromLeft (10);
        metronomeButton.setBounds (area.removeFromLeft (58).reduced (0, 6));
        area.removeFromLeft (4);
        countInBox.setBounds (area.removeFromLeft (130).reduced (0, 6));

        cpuLabel.setBounds (area.removeFromRight (74));
        area.removeFromRight (6);
        meterArea = area.removeFromRight (juce::jmin (200, juce::jmax (60, area.getWidth())))
                        .reduced (0, 7).toFloat();
        area.removeFromRight (10);
        outputDeviceBox.setBounds (area.removeFromRight (juce::jmin (160, juce::jmax (0, area.getWidth())))
                                        .reduced (0, 6));
    }
}
