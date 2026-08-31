#pragma once
#include "UI/UiSupport.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <map>

namespace ss
{
    class TimelineView;

    /** One track's controls, left of the arrangement (spec 9.2). */
    class TrackHeader final : public juce::Component
    {
    public:
        TrackHeader (AppContext&, UiState&, TrackId);

        TrackId getTrackId() const noexcept { return trackId; }
        void    refresh();

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        Track* track() const;
        void   showPluginChainMenu();
        void   showInputMenu();
        void   commit (const juce::String& name, std::function<void (Track&)> edit);

        AppContext& ctx;
        UiState&    ui;
        TrackId     trackId;

        juce::Label      nameLabel;
        juce::TextButton armButton, muteButton, soloButton, fxButton, inputButton, autoButton;
        juce::Slider     gainSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        juce::Slider     panSlider  { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        bool             updating = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
    };

    //==============================================================================
    /** Multi-track arrangement (spec 9.2): track headers on the left, a
        horizontally scrolling zoomable arrangement on the right showing audio
        clips as waveforms and MIDI clips as mini piano rolls on one bar/beat
        ruler.  Clips move, resize, split, snap and rubber-band select; audio and
        MIDI files can be dropped straight in; right-clicking an audio clip
        offers "Transcribe this range", which is the link into the AI loop. */
    class TimelineView final : public ProjectView,
                               private juce::Timer,
                               private juce::ScrollBar::Listener,
                               public juce::FileDragAndDropTarget,
                               public juce::DragAndDropTarget
    {
    public:
        TimelineView (AppContext&, UiState&);
        ~TimelineView() override;

        void zoom (double factor);
        /** Splits every selected clip at the playhead (Edit > Split). */
        void splitSelectionAtPlayhead();

        /** Preferences > General "Timeline redraw rate" - juce::Timer is a
            private base, so this is the settings-change hook's only way in. */
        void setRefreshHz (int hz) { startTimerHz (hz); }

        /** View > Automation: expands or collapses the selected track's
            automation lanes underneath it. */
        void toggleAutomationForSelectedTrack();

        /** Deletes the marquee/click selection of breakpoints.  Returns false
            when nothing was selected, so Edit > Delete can fall through to the
            clip it would otherwise have deleted. */
        bool deleteSelectedAutomationPoints();

        /** Whether Edit > Delete has breakpoints to work on.  The key mapping
            re-reads getCommandInfo() on every press, so this is live. */
        bool hasAutomationSelection() const noexcept { return ! selectedPoints.empty(); }

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        bool isInterestedInFileDrag (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray&, int x, int y) override;

        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

    private:
        struct ClipRef
        {
            TrackId track = invalidTrackId;
            ClipId  clip  = invalidClipId;
            bool    isAudio = false;
            bool operator== (const ClipRef& o) const noexcept
                { return track == o.track && clip == o.clip && isAudio == o.isAudio; }
        };

        /** One breakpoint.  Addressed by parameter id rather than lane index so
            it survives a lane being added or removed somewhere above it. */
        struct PointRef
        {
            TrackId      track = invalidTrackId;
            juce::String parameterId;
            int          index = -1;
            bool operator== (const PointRef& o) const noexcept
                { return track == o.track && parameterId == o.parameterId && index == o.index; }
        };

        enum class DragMode { none, move, resizeEnd, resizeStart, marquee, playhead, loop,
                              automationPoint, automationMarquee };

        void timerCallback() override;
        void scrollBarMoved (juce::ScrollBar*, double) override;

        void rebuildHeaders();
        void updateScrollRanges();
        void layOutHeaders();

        juce::Rectangle<int> rulerArea() const;
        juce::Rectangle<int> laneArea() const;
        juce::Rectangle<int> headerArea() const;
        juce::Rectangle<int> laneForTrackIndex (int index) const;
        int    trackIndexAt (int y) const;
        double beatForX (int x) const;
        float  xForBeat (double beat) const;

        // --- automation lanes (spec 8.4.5) ----------------------------------
        /** Rows shown under track `index`: one per lane plus the "add lane"
            row, or none when the track is collapsed. */
        int    automationRowsFor (int trackIndex) const;
        /** Recomputes blockTops.  Cheap, and called from layOutHeaders() so
            every path that moves something has fresh geometry. */
        void   updateLaneLayout();
        int    blockTopFor (int trackIndex) const;
        juce::Rectangle<int> automationRowRect (int trackIndex, int row) const;
        /** Which automation row a visible point is over.  `row` == the lane
            count means the "add lane" row. */
        bool   automationRowAt (juce::Point<int>, int& trackIndex, int& row) const;

        Track::AutomationLane* laneFor (TrackId, const juce::String& parameterId);
        static float yForValue (juce::Rectangle<int> plot, float value) noexcept;
        static float valueForY (juce::Rectangle<int> plot, int y) noexcept;
        int    pointIndexAt (const Track::AutomationLane&, juce::Rectangle<int> plot, juce::Point<int>) const;
        bool   isPointSelected (const PointRef&) const;
        juce::String laneLabel (const Track&, const juce::String& parameterId) const;
        /** The live instance behind track.plugins[slot], for parameter names. */
        juce::AudioPluginInstance* liveInstanceForSlot (const Track&, int slot) const;

        void paintAutomationCurves (juce::Graphics&);
        void paintAutomationHeaders (juce::Graphics&);
        void automationMouseDown (const juce::MouseEvent&, int trackIndex, int row);
        void dragAutomationPoints (const juce::MouseEvent&);
        void selectPointsIn (juce::Rectangle<int> area);
        void commitAutomationDrag();
        void commitAutomationAdd();
        /** `laneIndex` < 0 adds a lane; otherwise it re-points that lane. */
        void showParameterMenu (int trackIndex, int laneIndex, juce::Point<int> where);

        ClipRef clipAt (juce::Point<int>) const;
        juce::Rectangle<float> rectForClip (const ClipRef&) const;
        bool    isSelected (const ClipRef&) const;

        void paintAudioClip (juce::Graphics&, const Track&, const AudioClip&, juce::Rectangle<float>, bool selected);
        void paintMidiClip  (juce::Graphics&, const Track&, const MidiClip&,  juce::Rectangle<float>, bool selected);
        juce::AudioThumbnail* thumbnailFor (const juce::File&);

        void showClipMenu (const ClipRef&, juce::Point<int>);
        void showEmptyLaneMenu (int trackIndex, double beat, juce::Point<int>);
        void requestTranscribe (const ClipRef&, bool wholeClip);
        void importFiles (const juce::StringArray&, int trackIndex, double atBeat);
        /** Turns the live drag into one undoable step; `trackDelta` is how many
            lanes the clips were dropped away from where they started. */
        void commitClipDrag (int trackDelta);

        // --- view state -----------------------------------------------------
        double pixelsPerBeat = 26.0;
        double scrollBeats   = 0.0;
        int    scrollY       = 0;

        std::vector<ClipRef> selected;
        std::vector<std::unique_ptr<TrackHeader>> headers;

        DragMode dragMode = DragMode::none;
        bool     dragMoved = false;
        // Command/ctrl was held when this clip drag started, so a move past the
        // drag threshold exports the clip via DragAndDropContainer instead of
        // moving it within the timeline (Task 7).  exportClipRef is the exact
        // clip that was under the cursor at mouseDown - NOT necessarily
        // selected.front(), since a pre-existing multi-selection's first
        // entry can be a different clip (or an audio clip) than the one the
        // user actually pressed down on and is dragging.
        bool     exportingClipDrag = false;
        ClipRef  exportClipRef;
        juce::Point<int> dragStartPoint;
        double   dragStartBeat = 0.0;
        int      dragStartTrackIndex = 0;
        juce::Rectangle<int> marquee;

        struct DraggedClip { ClipRef ref; double startBeats, lengthBeats, offsetSeconds; int trackIndex; };
        std::vector<DraggedClip> dragOriginals;

        // --- automation state -----------------------------------------------
        std::vector<int> blockTops;          // content-space top of each track, + total at the end

        std::vector<PointRef> selectedPoints;
        struct DraggedPoint { PointRef ref; double beat; float value; };
        std::vector<DraggedPoint> pointOriginals;
        double dragStartValueY = 0.0;

        // A press on empty lane space is an add until the mouse moves, at which
        // point it becomes a rubber band instead.
        bool         pendingAdd = false;
        TrackId      pendingAddTrack = invalidTrackId;
        juce::String pendingAddParam;
        double       pendingAddBeat = 0.0;
        float        pendingAddValue = 0.0f;
        juce::Rectangle<int> pointMarquee;

        juce::AudioThumbnailCache thumbnailCache { 96 };
        std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails;

        juce::ScrollBar hScroll { false }, vScroll { true };
        juce::TextButton addAudioTrackButton, addMidiTrackButton;
        std::unique_ptr<juce::FileChooser> chooser;

        // Loop drag: the values to rewind to before the undoable commit.
        double loopStartBefore = 0.0, loopEndBefore = 0.0;
        bool   loopEnabledBefore = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
    };
}
