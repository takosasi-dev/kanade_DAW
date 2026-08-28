#pragma once
#include "AI/MusicTheory.h"
#include "Core/AppContext.h"
#include "Core/Project.h"
#include "Core/Types.h"
#include "UI/DarkLookAndFeel.h"
#include "UI/MainComponent.h"
#include <atomic>
#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>
#include <set>

namespace ss
{
    //==============================================================================
    /** Command ids.  The standard eight (quit/undo/redo/cut/copy/paste/delete/
        select-all) come from juce::StandardApplicationCommandIDs so the platform
        menus and text editors keep working; everything else lives here. */
    namespace CommandIDs
    {
        enum
        {
            fileNew = 0x3100, fileOpen, fileSave, fileSaveAs,
            importAudio, importMidi, exportMidi, exportAudio, exportMusicXml,
            addAudioTrack, addMidiTrack, removeSelectedTrack, splitClipAtPlayhead,
            transportPlay, transportStop, transportRecord, transportLoop,
            transportReturnToStart, toggleMetronome,
            viewTimeline, viewPianoRoll, viewMixer, viewTranscribe,
            viewGenerate, viewNotation, viewSession, viewModular,
            toggleBrowser, toggleAiPanel, zoomIn, zoomOut,
            transcribeSelection, generateFromSelection,
            showPreferences, rescanPlugins,
            // New ids go on the END: the values are what the user's saved key
            // mappings refer to, so inserting one in the middle silently
            // repoints every shortcut after it.
            toggleAutomation,
            exportDawProject, importDawProject
        };
    }

    //==============================================================================
    /** Runs `edit` inside one undoable transaction (spec 9.5 wants undo on
        everything, and Project exposes whole-document var serialisation but no
        per-field undoable model).

        // ponytail: whole-document snapshots via Project::toVar(). Correct for
        // every edit at O(project size) per action; swap in typed UndoableActions
        // if a project ever gets big enough for the copy to be felt. */
    void performProjectEdit (Project&, const juce::String& transactionName,
                             std::function<void()> edit);

    /** Push a value straight at the live mixer strip without touching the
        document.  Used while a fader is being dragged so it is audible; the
        document (and therefore the undo entry) is written once on release. */
    void applyLiveGain (AppContext&, TrackId, float gainDb);
    void applyLivePan  (AppContext&, TrackId, float pan);
    void applyLiveMute (AppContext&, TrackId, bool muted);

    //==============================================================================
    /** Purely visual state that more than one view needs to agree on: what is
        selected, where the grid is, which view to jump to next.  Not a
        view-model - views still read Project directly for everything real. */
    class UiState : public juce::ChangeBroadcaster
    {
    public:
        TrackId selectedTrack = invalidTrackId;
        ClipId  selectedClip  = invalidClipId;
        bool    selectedClipIsAudio = false;

        Quantise grid        = Quantise::sixteenth;
        bool     snapEnabled = true;

        /** Notes below this are "needs review" in the piano roll and transcribe
            views (spec 9.3). */
        float confidenceThreshold = 0.55f;

        theory::Key editKey;

        /** Set by TimelineView's right-click "Transcribe this range", consumed by
            TranscribeView - the spec calls this the link that makes the AI loop
            feel native (9.2). */
        struct TranscribeRequest
        {
            bool         valid = false;
            juce::File   file;
            double       offsetSeconds = 0.0, lengthSeconds = 0.0;
            double       placeAtBeat   = 0.0;
            juce::String clipName;
        };
        TranscribeRequest transcribeRequest;

        /** Installed by MainComponent so any view can switch the workspace. */
        std::function<void (MainComponent::View)> requestView;

        /** The clip currently being dragged out of the candidate gallery
            (spec 9.4 "drag it into the project").  Shared rather than copied into
            the drag description because juce::var has no room for a note list.
            // ponytail: one payload at a time, which is all a mouse can drag. */
        std::shared_ptr<MidiClip> dragPayload;

        void select (TrackId t, ClipId c, bool isAudio)
        {
            if (selectedTrack == t && selectedClip == c && selectedClipIsAudio == isAudio)
                return;
            selectedTrack = t; selectedClip = c; selectedClipIsAudio = isAudio;
            sendChangeMessage();
        }

        /** Installed by MainComponent so a view can fire an application command
            without having to be handed the command manager. */
        std::function<void (juce::CommandID)> invokeCommand;

        /** Tracks whose automation lanes are expanded in the timeline (spec
            8.4.5).  Purely visual, so it lives here rather than in the document:
            showing a lane is not an edit and must not dirty the project. */
        std::set<TrackId> automationTracks;

        bool isAutomationVisible (TrackId t) const noexcept
        {
            return automationTracks.find (t) != automationTracks.end();
        }

        void setAutomationVisible (TrackId t, bool shouldShow)
        {
            if (t == invalidTrackId)
                return;

            const bool changed = shouldShow ? automationTracks.insert (t).second
                                            : automationTracks.erase (t) > 0;
            if (changed)
                sendChangeMessage();
        }

        void goTo (MainComponent::View v) { if (requestView) requestView (v); }
        void invoke (juce::CommandID id)  { if (invokeCommand) invokeCommand (id); }

        /** Snaps to the current grid, honouring the snap toggle. */
        double snap (double beats) const noexcept
        {
            return snapEnabled ? applyQuantise (beats, grid, 1.0) : beats;
        }
    };

    //==============================================================================
    /** Base for every workspace view: holds the context refs and re-points its
        ChangeListener registration when the document is replaced.

        Views MUST be detached before AppContext::setProject() destroys the old
        Project and re-attached afterwards; MainComponent does exactly that. */
    class ProjectView : public juce::Component,
                        public juce::ChangeListener
    {
    public:
        ProjectView (AppContext& c, UiState& s);
        ~ProjectView() override;

        /** Virtual so a view that embeds another view can forward the call. */
        virtual void detachFromProject();
        virtual void attachToProject();

    protected:
        Project& project() const noexcept { return *ctx.project; }
        AppContext& ctx;
        UiState&    ui;

    private:
        Project* listeningTo = nullptr;
    };

    //==============================================================================
    /** Inline progress strip for the long jobs the spec insists must not block
        the UI: transcription, generation, stem separation, rendering, scanning.

        The work lambda runs on a private background thread and talks back only
        through the atomics here; the completion lambda is bounced onto the
        message thread. */
    class TaskPanel final : public juce::Component,
                            private juce::Timer
    {
    public:
        TaskPanel();
        ~TaskPanel() override;

        /** Starts `work` on a background thread.  Does nothing (returns false) if
            a job is already running - one panel drives one job. */
        bool run (const juce::String& title,
                  std::function<void (TaskPanel&)> work,
                  std::function<void()> onFinished);

        void cancel();
        bool isBusy() const noexcept { return busy.load(); }

        /** Called on the message thread when the user hits Cancel, for jobs that
            need more than the flag (killing a child process, say). */
        std::function<void()> onCancel;

        // --- called from the background thread ---
        void setProgress (float p) noexcept { progress.store (juce::jlimit (0.0f, 1.0f, p)); }
        void setStatus (const juce::String&);
        bool isCancelled() const noexcept { return cancelled.load(); }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        class Worker;
        void timerCallback() override;

        std::unique_ptr<Worker> worker;
        std::function<void()>   finishedCallback;

        juce::Label       titleLabel, statusLabel;
        juce::TextButton  cancelButton;
        double            barValue = 0.0;
        juce::ProgressBar bar { barValue };

        std::atomic<float> progress { 0.0f };
        std::atomic<bool>  cancelled { false }, busy { false };

        juce::CriticalSection statusLock;
        juce::String          statusText;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TaskPanel)
    };

    //==============================================================================
    // Small painting / formatting helpers shared by the timeline-ish views.

    /** bar:beat:tick display for a beat position, using the project time sig. */
    juce::String formatBarBeat (const TempoMap&, double beats);
    /** h:mm:ss.mmm for a beat position. */
    juce::String formatTimecode (const TempoMap&, double beats);

    /** Draws the bar / beat / subdivision grid over `area`, which maps
        [scrollBeats, scrollBeats + area.width / pixelsPerBeat). */
    void paintBeatGrid (juce::Graphics&, juce::Rectangle<int> area, const TempoMap&,
                        double scrollBeats, double pixelsPerBeat, Quantise subdivision);

    /** The bar/beat ruler strip shared by the timeline and the editors. */
    void paintBeatRuler (juce::Graphics&, juce::Rectangle<int> area, const TempoMap&,
                         double scrollBeats, double pixelsPerBeat);

    /** A peak/RMS meter.  `rms` < 0 draws peak only. */
    void paintMeter (juce::Graphics&, juce::Rectangle<float> area, float peak, float rms,
                     bool vertical);

    /** dB text that never says "-inf dB" in a 40px wide label. */
    juce::String formatDb (float db);

    /** Quantise values in menu order, shared by every grid combo box in the app. */
    const std::vector<Quantise>& quantiseMenuValues();
    void fillQuantiseComboBox (juce::ComboBox&, Quantise selected);
    Quantise quantiseFromComboBox (const juce::ComboBox&);

    /** Scale types in menu order, for the key pickers. */
    const std::vector<theory::ScaleType>& scaleMenuValues();
    void fillKeyComboBoxes (juce::ComboBox& root, juce::ComboBox& scale, const theory::Key&);
    theory::Key keyFromComboBoxes (const juce::ComboBox& root, const juce::ComboBox& scale);

    /** Converts a linear 0..1 magnitude to the 0..1 position on a meter scale
        that gives -60..0 dB a usable amount of travel. */
    float meterScale (float linearMagnitude) noexcept;
}
