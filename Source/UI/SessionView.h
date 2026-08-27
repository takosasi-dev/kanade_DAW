#pragma once
#include "UI/UiSupport.h"
#include <map>

namespace ss
{
    class SessionView;

    /** One (track, scene) cell in the grid: empty (drop target only) or
        filled (shows the clip's name, coloured by its owning track). */
    class SessionCell final : public juce::Component,
                              public juce::DragAndDropTarget
    {
    public:
        SessionCell (SessionView& ownerView, TrackId trackId, SceneId sceneId);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

    private:
        SessionView& owner;
        TrackId trackId;
        SceneId sceneId;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionCell)
    };

    /** Left-edge launch/stop toggle + name for one scene row. */
    class SceneRowHeader final : public juce::Component
    {
    public:
        SceneRowHeader (SessionView& ownerView, SceneId sceneId);

        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        SessionView& owner;
        SceneId sceneId;
        juce::TextButton launchButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneRowHeader)
    };

    /** Read-only mirror of a track's name/colour along the grid's top edge -
        editing stays in MixerView/TimelineView, the one place each control is
        owned (spec: Session view, Phase 3). */
    class SessionTrackHeader final : public juce::Component
    {
    public:
        SessionTrackHeader (SessionView& ownerView, TrackId trackId);

        void paint (juce::Graphics&) override;

    private:
        SessionView& owner;
        TrackId trackId;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionTrackHeader)
    };

    //==============================================================================
    /** Track x scene clip launcher (spec: Session view, Phase 3). */
    class SessionView final : public ProjectView,
                              private juce::Timer,
                              public juce::DragAndDropTarget
    {
    public:
        SessionView (AppContext&, UiState&);
        ~SessionView() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        void attachToProject() override;

        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

        // --- used by SessionCell / SceneRowHeader ------------------------------
        Track* trackById (TrackId) noexcept;
        void addScene();
        void renameScene (SceneId, const juce::String& newName);
        void deleteScene (SceneId);
        void toggleSceneRow (SceneId);
        void launchCell (TrackId, SceneId);
        Project* getProject() const noexcept { return ctx.project.get(); }
        AudioEngine* getEngine() const noexcept { return ctx.engine.get(); }

        /** Applies a dropped clip's payload to one cell.  Public and taking
            the drop's TrackId/SceneId explicitly (rather than only the
            SourceDetails) so tests can drive it without a real drag
            gesture, matching this project's established `DockTests.cpp`
            pattern of calling handlers directly. */
        void handleClipDrop (const SourceDetails&, TrackId, SceneId);

        static constexpr int headerHeight = 28;
        static constexpr int rowHeight    = 56;
        static constexpr int columnWidth  = 120;
        static constexpr int sceneColumnWidth = 110;

    private:
        void rebuildGrid();
        void timerCallback() override;

        juce::TextButton addSceneButton;
        juce::OwnedArray<SessionTrackHeader> trackHeaders;
        juce::OwnedArray<SceneRowHeader>     sceneHeaders;
        juce::OwnedArray<SessionCell>        cells;
        juce::Viewport gridViewport;
        juce::Component gridHolder;

        // Exactly the track/scene counts rebuildGrid() just used to build cells/
        // trackHeaders/sceneHeaders - resized() must index using THESE, never a
        // live re-read of project().getTracks()/scenes.size(), since Project's
        // change broadcast that triggers the next rebuildGrid() is asynchronous:
        // a caller can grow the live project and synchronously force a resized()
        // (MainComponent::showView -> setBounds, on the "Send to Session" path)
        // before rebuildGrid() has had a chance to catch up, and indexing `cells`
        // with stale-but-larger counts is a null-pointer crash (OwnedArray::
        // operator[] returns nullptr out of range).
        int gridColumns = 0;
        int gridRows    = 0;

        // Which scene's clip launchCell last launched on each track - isSessionClipActive/
        // isSessionClipQueued only know "something is active on this track", not which scene
        // it came from, so this is what lets launchCell tell "click the active cell again to
        // stop it" apart from "click a different filled cell on the same track to switch to
        // that scene's clip" (the latter must launch/replace, never stop).
        std::map<TrackId, SceneId> activeSceneForTrack;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionView)
    };
}
