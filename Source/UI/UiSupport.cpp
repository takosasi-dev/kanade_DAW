#include "UI/UiSupport.h"
#include "Engine/AudioEngine.h"
#include "Mixer/Mixer.h"
#include <cmath>

namespace ss
{
    namespace
    {
        ChannelStrip* stripFor (AppContext& ctx, TrackId id)
        {
            return ctx.engine != nullptr ? ctx.engine->getMixer().getStripForTrack (id) : nullptr;
        }
    }

    void applyLiveGain (AppContext& ctx, TrackId id, float gainDb)
    {
        if (auto* s = stripFor (ctx, id)) s->setGainDb (gainDb);
    }

    void applyLivePan (AppContext& ctx, TrackId id, float pan)
    {
        if (auto* s = stripFor (ctx, id)) s->setPan (pan);
    }

    void applyLiveMute (AppContext& ctx, TrackId id, bool muted)
    {
        if (auto* s = stripFor (ctx, id)) s->setMuted (muted);
    }

    //==============================================================================
    namespace
    {
        /** Snapshots the document either side of an edit.  See the header for why
            this is a whole-document copy rather than a typed action. */
        class ProjectSnapshotAction final : public juce::UndoableAction
        {
        public:
            ProjectSnapshotAction (Project& p, std::function<void()> e)
                : proj (p), edit (std::move (e))
            {
                before = proj.toVar();
            }

            bool perform() override
            {
                if (isFirstRun)
                {
                    if (edit) edit();
                    after = proj.toVar();
                    isFirstRun = false;
                }
                else
                {
                    proj.loadFromVar (after);
                }

                proj.markDirty();
                proj.sendChangeMessage();
                return true;
            }

            bool undo() override
            {
                proj.loadFromVar (before);
                proj.markDirty();
                proj.sendChangeMessage();
                return true;
            }

            int getSizeInUnits() override { return 2048; }

        private:
            Project& proj;
            std::function<void()> edit;
            juce::var before, after;
            bool isFirstRun = true;
        };
    }

    void performProjectEdit (Project& p, const juce::String& transactionName,
                             std::function<void()> edit)
    {
        auto& um = p.getUndoManager();
        um.beginNewTransaction (transactionName);
        um.perform (new ProjectSnapshotAction (p, std::move (edit)), transactionName);
    }

    //==============================================================================
    ProjectView::ProjectView (AppContext& c, UiState& s)
        : ctx (c), ui (s)
    {
        ui.addChangeListener (this);
        attachToProject();
    }

    ProjectView::~ProjectView()
    {
        ui.removeChangeListener (this);
        detachFromProject();
    }

    void ProjectView::detachFromProject()
    {
        if (listeningTo != nullptr)
        {
            listeningTo->removeChangeListener (this);
            listeningTo = nullptr;
        }
    }

    void ProjectView::attachToProject()
    {
        detachFromProject();

        if (ctx.project != nullptr)
        {
            listeningTo = ctx.project.get();
            listeningTo->addChangeListener (this);
        }
    }

    //==============================================================================
    class TaskPanel::Worker final : public juce::Thread
    {
    public:
        Worker (TaskPanel& o, std::function<void (TaskPanel&)> w)
            : juce::Thread ("ScoreSmith Task"), owner (o), work (std::move (w)) {}

        void run() override { if (work) work (owner); }

    private:
        TaskPanel& owner;
        std::function<void (TaskPanel&)> work;
    };

    TaskPanel::TaskPanel()
    {
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setColour (juce::Label::textColourId, palette().textDim);
        cancelButton.setButtonText (TRANS ("Cancel"));
        cancelButton.onClick = [this] { cancel(); };

        addAndMakeVisible (titleLabel);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (bar);
        addAndMakeVisible (cancelButton);
        setVisible (false);
    }

    TaskPanel::~TaskPanel()
    {
        stopTimer();

        if (worker != nullptr)
        {
            cancelled.store (true);
            worker->stopThread (4000);
            worker.reset();
        }
    }

    bool TaskPanel::run (const juce::String& title,
                         std::function<void (TaskPanel&)> work,
                         std::function<void()> onFinished)
    {
        if (busy.load())
            return false;

        cancelled.store (false);
        progress.store (0.0f);
        setStatus ({});
        titleLabel.setText (title, juce::dontSendNotification);
        finishedCallback = std::move (onFinished);

        worker = std::make_unique<Worker> (*this, std::move (work));

        if (! worker->startThread())
        {
            worker.reset();
            finishedCallback = {};
            return false;
        }

        busy.store (true);
        setVisible (true);
        toFront (false);
        startTimerHz (20);
        return true;
    }

    void TaskPanel::cancel()
    {
        cancelled.store (true);
        setStatus (TRANS ("Cancelling..."));
        if (onCancel) onCancel();
    }

    void TaskPanel::setStatus (const juce::String& s)
    {
        const juce::ScopedLock sl (statusLock);
        statusText = s;
    }

    void TaskPanel::timerCallback()
    {
        barValue = (double) progress.load();
        bar.repaint();

        {
            const juce::ScopedLock sl (statusLock);
            if (statusLabel.getText() != statusText)
                statusLabel.setText (statusText, juce::dontSendNotification);
        }

        if (worker != nullptr && ! worker->isThreadRunning())
        {
            stopTimer();
            worker->stopThread (2000);
            worker.reset();
            busy.store (false);
            setVisible (false);

            // Move it out first: the callback is allowed to start the next job.
            auto callback = std::move (finishedCallback);
            finishedCallback = {};
            if (callback) callback();
        }
    }

    void TaskPanel::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (p.panelAltBg);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (p.accent.withAlpha (0.7f));
        g.drawRoundedRectangle (area, 4.0f, 1.0f);
    }

    void TaskPanel::resized()
    {
        auto area = getLocalBounds().reduced (8, 6);
        cancelButton.setBounds (area.removeFromRight (84).reduced (0, 2));
        area.removeFromRight (8);
        titleLabel.setBounds (area.removeFromTop (juce::jmax (16, area.getHeight() / 3)));
        bar.setBounds (area.removeFromTop (juce::jmax (10, area.getHeight() / 2)).reduced (0, 1));
        statusLabel.setBounds (area);
    }

    //==============================================================================
    juce::String formatBarBeat (const TempoMap& tempo, double beats)
    {
        int bar = 1;
        double beatInBar = 0.0;
        tempo.barAndBeat (beats, bar, beatInBar);

        const int beatNumber = (int) std::floor (beatInBar) + 1;
        const int ticks      = (int) std::floor ((beatInBar - std::floor (beatInBar)) * 960.0);

        return juce::String (bar) + "." + juce::String (beatNumber)
             + "." + juce::String (ticks).paddedLeft ('0', 3);
    }

    juce::String formatTimecode (const TempoMap& tempo, double beats)
    {
        auto seconds = juce::jmax (0.0, tempo.beatsToSeconds (beats));
        const int totalMs = (int) std::floor (seconds * 1000.0 + 0.5);
        const int ms  = totalMs % 1000;
        const int s   = (totalMs / 1000) % 60;
        const int m   = (totalMs / 60000) % 60;
        const int h   = totalMs / 3600000;

        return juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2)
             + ":" + juce::String (s).paddedLeft ('0', 2)
             + "." + juce::String (ms).paddedLeft ('0', 3);
    }

    void paintBeatGrid (juce::Graphics& g, juce::Rectangle<int> area, const TempoMap& tempo,
                        double scrollBeats, double pixelsPerBeat, Quantise subdivision)
    {
        if (area.isEmpty() || pixelsPerBeat <= 0.0)
            return;

        const auto& p = palette();
        const double endBeats = scrollBeats + area.getWidth() / pixelsPerBeat;

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area);

        auto xFor = [&] (double beat) { return (float) (area.getX() + (beat - scrollBeats) * pixelsPerBeat); };

        // Subdivisions, but only while they are far enough apart to be readable.
        const double step = quantiseStepInBeats (subdivision);
        if (step > 0.0 && step * pixelsPerBeat >= 7.0)
        {
            g.setColour (p.gridSub);
            for (double b = std::floor (scrollBeats / step) * step; b < endBeats; b += step)
                g.drawVerticalLine (juce::roundToInt (xFor (b)), (float) area.getY(), (float) area.getBottom());
        }

        if (pixelsPerBeat >= 6.0)
        {
            g.setColour (p.gridBeat);
            for (double b = std::floor (scrollBeats); b < endBeats; b += 1.0)
                g.drawVerticalLine (juce::roundToInt (xFor (b)), (float) area.getY(), (float) area.getBottom());
        }

        // Bar lines walk the time-signature map rather than assuming 4/4.
        g.setColour (p.gridBar);
        int barNumber = 1;
        double beatInBar = 0.0;
        tempo.barAndBeat (juce::jmax (0.0, scrollBeats), barNumber, beatInBar);

        double b = juce::jmax (0.0, scrollBeats) - beatInBar;
        for (int guard = 0; b < endBeats && guard < 8192; ++guard)
        {
            g.drawVerticalLine (juce::roundToInt (xFor (b)), (float) area.getY(), (float) area.getBottom());
            const auto ts = tempo.timeSignatureAt (b);
            const double barLength = ts.numerator * (4.0 / juce::jmax (1, ts.denominator));
            if (barLength <= 0.0) break;
            b += barLength;
        }
    }

    void paintBeatRuler (juce::Graphics& g, juce::Rectangle<int> area, const TempoMap& tempo,
                         double scrollBeats, double pixelsPerBeat)
    {
        if (area.isEmpty() || pixelsPerBeat <= 0.0)
            return;

        const auto& p = palette();
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area);

        g.setColour (p.headerBg);
        g.fillRect (area);
        g.setColour (p.outline);
        g.drawHorizontalLine (area.getBottom() - 1, (float) area.getX(), (float) area.getRight());

        const double endBeats = scrollBeats + area.getWidth() / pixelsPerBeat;
        auto xFor = [&] (double beat) { return (float) (area.getX() + (beat - scrollBeats) * pixelsPerBeat); };

        int barNumber = 1;
        double beatInBar = 0.0;
        tempo.barAndBeat (juce::jmax (0.0, scrollBeats), barNumber, beatInBar);

        double b = juce::jmax (0.0, scrollBeats) - beatInBar;
        g.setFont (juce::Font (juce::FontOptions (11.0f)));

        for (int guard = 0; b < endBeats && guard < 4096; ++guard)
        {
            const auto ts = tempo.timeSignatureAt (b);
            const double barLength = ts.numerator * (4.0 / juce::jmax (1, ts.denominator));
            const float x = xFor (b);

            g.setColour (p.gridBar);
            g.drawVerticalLine (juce::roundToInt (x), (float) area.getY() + 4.0f, (float) area.getBottom());

            // Only label bars that have room for the text.
            if (barLength * pixelsPerBeat >= 26.0)
            {
                g.setColour (p.textDim);
                g.drawText (juce::String (barNumber), juce::roundToInt (x) + 3, area.getY(),
                            60, area.getHeight() - 2, juce::Justification::centredLeft, false);
            }

            // Beat ticks inside the bar when the bar is wide enough.
            if (pixelsPerBeat >= 14.0)
            {
                g.setColour (p.gridBeat);
                for (int beat = 1; beat < ts.numerator; ++beat)
                {
                    const float bx = xFor (b + beat * (4.0 / juce::jmax (1, ts.denominator)));
                    g.drawVerticalLine (juce::roundToInt (bx), (float) area.getBottom() - 6.0f,
                                        (float) area.getBottom());
                }
            }

            if (barLength <= 0.0) break;
            b += barLength;
            ++barNumber;
        }
    }

    float meterScale (float linearMagnitude) noexcept
    {
        const auto db = juce::Decibels::gainToDecibels (juce::jmax (1.0e-6f, linearMagnitude), -60.0f);
        return juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));
    }

    void paintMeter (juce::Graphics& g, juce::Rectangle<float> area, float peak, float rms,
                     bool vertical)
    {
        const auto& p = palette();
        g.setColour (p.laneBg);
        g.fillRect (area);

        const float peakPos = meterScale (peak);

        if (peakPos > 0.0f)
        {
            auto filled = area;
            if (vertical) filled = filled.withTop (area.getBottom() - area.getHeight() * peakPos);
            else          filled = filled.withWidth (area.getWidth() * peakPos);

            g.setColour (meterColour (peakPos).withAlpha (0.55f));
            g.fillRect (filled);
        }

        if (rms >= 0.0f)
        {
            const float rmsPos = meterScale (rms);
            auto filled = area;
            if (vertical) filled = filled.withTop (area.getBottom() - area.getHeight() * rmsPos);
            else          filled = filled.withWidth (area.getWidth() * rmsPos);

            g.setColour (meterColour (rmsPos));
            g.fillRect (filled);
        }

        // -6 dB and 0 dB marks so the meter can be read without a scale legend.
        g.setColour (p.outline.withAlpha (0.8f));
        for (float db : { -6.0f, -18.0f })
        {
            const float pos = juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f);
            if (vertical) g.drawHorizontalLine (juce::roundToInt (area.getBottom() - area.getHeight() * pos),
                                                area.getX(), area.getRight());
            else          g.drawVerticalLine (juce::roundToInt (area.getX() + area.getWidth() * pos),
                                              area.getY(), area.getBottom());
        }

        g.setColour (p.outline);
        g.drawRect (area, 1.0f);
    }

    juce::String formatDb (float db)
    {
        if (db <= -59.9f) return "-" + juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"));
        return juce::String (db, db > -10.0f ? 1 : 0) + " dB";
    }

    //==============================================================================
    const std::vector<Quantise>& quantiseMenuValues()
    {
        static const std::vector<Quantise> values
        {
            Quantise::off, Quantise::whole, Quantise::half, Quantise::quarter, Quantise::eighth,
            Quantise::sixteenth, Quantise::thirtySecond, Quantise::eighthTriplet, Quantise::sixteenthTriplet
        };
        return values;
    }

    void fillQuantiseComboBox (juce::ComboBox& box, Quantise selected)
    {
        const auto& values = quantiseMenuValues();
        box.clear (juce::dontSendNotification);

        for (int i = 0; i < (int) values.size(); ++i)
            box.addItem (toString (values[(size_t) i]), i + 1);

        for (int i = 0; i < (int) values.size(); ++i)
            if (values[(size_t) i] == selected)
                box.setSelectedId (i + 1, juce::dontSendNotification);

        if (box.getSelectedId() == 0)
            box.setSelectedId (1, juce::dontSendNotification);
    }

    Quantise quantiseFromComboBox (const juce::ComboBox& box)
    {
        const auto& values = quantiseMenuValues();
        const int index = juce::jlimit (0, (int) values.size() - 1, box.getSelectedId() - 1);
        return values[(size_t) index];
    }

    const std::vector<theory::ScaleType>& scaleMenuValues()
    {
        using S = theory::ScaleType;
        static const std::vector<S> values
        {
            S::major, S::naturalMinor, S::harmonicMinor, S::dorian, S::mixolydian, S::lydian,
            S::phrygian, S::locrian, S::majorPentatonic, S::minorPentatonic, S::blues, S::chromatic
        };
        return values;
    }

    void fillKeyComboBoxes (juce::ComboBox& root, juce::ComboBox& scale, const theory::Key& key)
    {
        const auto names = theory::noteNames();
        root.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i)
            root.addItem (names[i], i + 1);
        root.setSelectedId (juce::jlimit (0, 11, key.tonic) + 1, juce::dontSendNotification);

        const auto& scales = scaleMenuValues();
        scale.clear (juce::dontSendNotification);
        for (int i = 0; i < (int) scales.size(); ++i)
            scale.addItem (theory::toString (scales[(size_t) i]), i + 1);

        scale.setSelectedId (1, juce::dontSendNotification);
        for (int i = 0; i < (int) scales.size(); ++i)
            if (scales[(size_t) i] == key.scale)
                scale.setSelectedId (i + 1, juce::dontSendNotification);
    }

    theory::Key keyFromComboBoxes (const juce::ComboBox& root, const juce::ComboBox& scale)
    {
        const auto& scales = scaleMenuValues();
        theory::Key key;
        key.tonic = juce::jlimit (0, 11, root.getSelectedId() - 1);
        key.scale = scales[(size_t) juce::jlimit (0, (int) scales.size() - 1, scale.getSelectedId() - 1)];
        return key;
    }
}
