#include "UI/SessionView.h"
#include "Engine/AudioEngine.h"

namespace ss
{
    //==============================================================================
    SessionCell::SessionCell (SessionView& ownerView, TrackId t, SceneId s)
        : owner (ownerView), trackId (t), sceneId (s)
    {
    }

    void SessionCell::paint (juce::Graphics& g)
    {
        auto bounds = getLocalBounds().reduced (2);
        auto* track = owner.trackById (trackId);
        auto* clip  = track != nullptr ? track->findSessionClip (sceneId) : nullptr;

        if (clip == nullptr)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.3f));
            g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 1.0f);
            return;
        }

        const auto colour = track != nullptr ? track->colour : juce::Colours::grey;
        g.setColour (colour.withAlpha (0.6f));
        g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

        // ponytail: active/queued reflects the WHOLE track, not this specific
        // cell - a track with clips in more than one scene shows every one of
        // its filled cells as active/queued together, since nothing yet
        // records which scene id a track's currently-loaded clip came from.
        // Precise per-cell attribution needs SessionView to remember "the
        // last scene id launched per track"; add that if this proves
        // confusing in practice.
        if (auto* engine = owner.getEngine())
        {
            if (engine->isSessionClipActive (trackId))
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 2.0f);
            }
            else if (engine->isSessionClipQueued (trackId))
            {
                g.setColour (juce::Colours::white.withAlpha (0.5f));
                g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 1.0f);
            }
        }

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (clip->name, bounds.reduced (4), juce::Justification::centredLeft, 2);
    }

    void SessionCell::mouseDown (const juce::MouseEvent&)
    {
        owner.launchCell (trackId, sceneId);
    }

    bool SessionCell::isInterestedInDragSource (const SourceDetails& details)
    {
        return details.description.toString() == "ss.timelineClip"
            || details.description.toString() == "ss.candidate";
    }

    void SessionCell::itemDropped (const SourceDetails& details)
    {
        owner.handleClipDrop (details, trackId, sceneId);
    }

    //==============================================================================
    SceneRowHeader::SceneRowHeader (SessionView& ownerView, SceneId s)
        : owner (ownerView), sceneId (s)
    {
        addAndMakeVisible (launchButton);
        launchButton.onClick = [this] { owner.toggleSceneRow (sceneId); };

        // The button covers almost the whole row, so a right-click on the scene
        // name would otherwise never reach mouseDown() below - forward it here.
        launchButton.addMouseListener (this, false);

        if (auto* p = owner.getProject())
            if (auto* scene = p->findScene (s))
                launchButton.setButtonText (scene->name);
    }

    void SceneRowHeader::resized()
    {
        launchButton.setBounds (getLocalBounds().reduced (4));
    }

    void SceneRowHeader::mouseDown (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem ("Rename", [this]
            {
                auto* aw = new juce::AlertWindow (TRANS ("Rename Scene"), {}, juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor ("name", launchButton.getButtonText());
                aw->addButton (TRANS ("OK"), 1);
                aw->addButton (TRANS ("Cancel"), 0);
                aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int result)
                {
                    if (result == 1)
                        owner.renameScene (sceneId, aw->getTextEditorContents ("name"));
                    delete aw;
                }));
            });
            menu.addItem ("Delete", [this] { owner.deleteScene (sceneId); });
            menu.showMenuAsync (juce::PopupMenu::Options());
        }
    }

    //==============================================================================
    SessionTrackHeader::SessionTrackHeader (SessionView& ownerView, TrackId t)
        : owner (ownerView), trackId (t)
    {
    }

    void SessionTrackHeader::paint (juce::Graphics& g)
    {
        auto* track = owner.trackById (trackId);

        // A track can vanish from under an existing header the same way a cell's
        // clip can (deleted, undo) - fall back to a neutral swatch and no text
        // rather than crash, mirroring SessionCell::paint's null handling.
        g.fillAll ((track != nullptr ? track->colour : juce::Colours::grey).withAlpha (0.3f));
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (track != nullptr ? track->name : juce::String(),
                          getLocalBounds().reduced (4), juce::Justification::centred, 1);
    }

    //==============================================================================
    SessionView::SessionView (AppContext& c, UiState& s) : ProjectView (c, s)
    {
        addAndMakeVisible (addSceneButton);
        addSceneButton.setButtonText (TRANS ("+ Scene"));
        addSceneButton.onClick = [this] { addScene(); };

        addAndMakeVisible (gridViewport);
        gridViewport.setViewedComponent (&gridHolder, false);

        rebuildGrid();
        startTimerHz (10);
    }

    SessionView::~SessionView() = default;

    Track* SessionView::trackById (TrackId trackId) noexcept
    {
        return ctx.project != nullptr ? ctx.project->findTrack (trackId) : nullptr;
    }

    void SessionView::addScene()
    {
        if (ctx.project == nullptr)
            return;

        performProjectEdit (project(), TRANS ("Add scene"), [this]
        {
            project().addScene (TRANS ("Scene") + " " + juce::String (project().scenes.size() + 1));
        });
    }

    void SessionView::renameScene (SceneId sceneId, const juce::String& newName)
    {
        if (ctx.project == nullptr || newName.isEmpty())
            return;

        performProjectEdit (project(), TRANS ("Rename scene"), [this, sceneId, newName]
        {
            if (auto* scene = project().findScene (sceneId))
                scene->name = newName;
        });
    }

    void SessionView::deleteScene (SceneId sceneId)
    {
        if (ctx.project == nullptr)
            return;

        performProjectEdit (project(), TRANS ("Delete scene"), [this, sceneId]
        {
            project().removeScene (sceneId);
        });
    }

    void SessionView::launchCell (TrackId trackId, SceneId sceneId)
    {
        if (ctx.project == nullptr || ctx.engine == nullptr)
            return;

        auto* track = trackById (trackId);
        auto* clip  = track != nullptr ? track->findSessionClip (sceneId) : nullptr;

        if (clip == nullptr)
            return;

        const auto it = activeSceneForTrack.find (trackId);
        const bool clickedTheAlreadyLaunchedScene = it != activeSceneForTrack.end() && it->second == sceneId;

        // isSessionClipActive/isSessionClipQueued only know "something is running on this
        // track", not which scene it came from - activeSceneForTrack is what tells apart
        // "click the currently-launched cell again -> stop" from "click a DIFFERENT filled
        // cell on a track that already has something playing -> launch/replace it" (the
        // engine's launchSessionClip already replaces whatever was running on that track,
        // see Task 4; this is just making sure launchCell actually calls it in that case
        // instead of stopping).
        if (clickedTheAlreadyLaunchedScene
            && (ctx.engine->isSessionClipActive (trackId) || ctx.engine->isSessionClipQueued (trackId)))
        {
            ctx.engine->stopSessionClip (trackId);
            activeSceneForTrack.erase (trackId);
        }
        else
        {
            ctx.engine->launchSessionClip (trackId, *clip);
            activeSceneForTrack[trackId] = sceneId;
        }
    }

    void SessionView::toggleSceneRow (SceneId sceneId)
    {
        if (ctx.project == nullptr)
            return;

        for (const auto& t : project().getTracks())
            if (t->findSessionClip (sceneId) != nullptr)
                launchCell (t->getId(), sceneId);
    }

    void SessionView::timerCallback()
    {
        repaint();
    }

    void SessionView::rebuildGrid()
    {
        trackHeaders.clear();
        sceneHeaders.clear();
        cells.clear();
        gridColumns = 0;
        gridRows    = 0;

        if (ctx.project != nullptr)
        {
            const auto& tracks = project().getTracks();

            for (const auto& t : tracks)
            {
                auto* header = trackHeaders.add (new SessionTrackHeader (*this, t->getId()));
                gridHolder.addAndMakeVisible (header);
            }

            for (const auto& scene : project().scenes)
            {
                auto* header = sceneHeaders.add (new SceneRowHeader (*this, scene.id));
                gridHolder.addAndMakeVisible (header);
            }

            for (const auto& scene : project().scenes)
                for (const auto& t : tracks)
                    gridHolder.addAndMakeVisible (cells.add (new SessionCell (*this, t->getId(), scene.id)));

            // rebuildGrid() saves these itself, rather than resized() re-reading
            // project() live, so resized() can never index further into `cells`
            // than what was actually just built here (see the crash this fixes
            // at gridColumns/gridRows's declaration in SessionView.h).
            gridColumns = (int) tracks.size();
            gridRows    = (int) project().scenes.size();
        }

        // A scene delete, a track delete, or an undo of either can remove the
        // very (track, scene) pair whose clip the engine is still looping -
        // Mixer keeps its own independent copy in a ring buffer and has no idea
        // the document changed. Once that happens, launchCell can no longer
        // reach the cell at all (findSessionClip comes back null and it early-
        // returns), so without this reconciliation the loop is unstoppable
        // short of restarting the app. rebuildGrid() already runs on every
        // project change (changeListenerCallback(), and now attachToProject()
        // too), so this one pass catches scene deletion, track deletion and
        // undo of any of the above uniformly.
        for (auto it = activeSceneForTrack.begin(); it != activeSceneForTrack.end(); )
        {
            auto* track = trackById (it->first);
            const bool stillValid = track != nullptr && track->findSessionClip (it->second) != nullptr;

            if (stillValid)
            {
                ++it;
            }
            else
            {
                if (ctx.engine != nullptr)
                    ctx.engine->stopSessionClip (it->first);

                it = activeSceneForTrack.erase (it);
            }
        }

        resized();
    }

    void SessionView::paint (juce::Graphics& g)
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
    }

    void SessionView::resized()
    {
        auto area = getLocalBounds();
        addSceneButton.setBounds (area.removeFromBottom (28).removeFromLeft (100).reduced (2));
        gridViewport.setBounds (area);

        // gridColumns/gridRows are what rebuildGrid() actually built `cells` for -
        // NOT a live re-read of project().getTracks()/scenes.size(), which can
        // already reflect a change rebuildGrid() hasn't processed yet (Project's
        // change broadcast is asynchronous). Indexing with a live count here is
        // what used to crash on the "Send to Session" path.
        gridHolder.setSize (sceneColumnWidth + gridColumns * columnWidth,
                            headerHeight + gridRows * rowHeight);

        for (int i = 0; i < trackHeaders.size(); ++i)
            trackHeaders[i]->setBounds (sceneColumnWidth + i * columnWidth, 0, columnWidth, headerHeight);

        for (int row = 0; row < sceneHeaders.size(); ++row)
            sceneHeaders[row]->setBounds (0, headerHeight + row * rowHeight, sceneColumnWidth, rowHeight);

        for (int row = 0; row < gridRows; ++row)
            for (int col = 0; col < gridColumns; ++col)
                cells[row * gridColumns + col]->setBounds (sceneColumnWidth + col * columnWidth,
                                                           headerHeight + row * rowHeight,
                                                           columnWidth, rowHeight);
    }

    void SessionView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        rebuildGrid();
        repaint();
    }

    void SessionView::attachToProject()
    {
        ProjectView::attachToProject();

        // Makes the grid reflect the newly-attached project's tracks/scenes
        // immediately, rather than only after the next async change broadcast -
        // closes the same gap as the resized() crash fix above (both are "grid
        // must reflect project state right after any known attach point").
        rebuildGrid();

        // A swapped-in Project starts its own TrackId numbering from scratch (see
        // Project::addTrack's lastTrackId), so a leftover entry here could otherwise
        // be misread as "this brand new track already has scene X launched".
        activeSceneForTrack.clear();
    }

    bool SessionView::isInterestedInDragSource (const SourceDetails& details)
    {
        return details.description.toString() == "ss.timelineClip"
            || details.description.toString() == "ss.candidate";
    }

    void SessionView::itemDropped (const SourceDetails& details)
    {
        // The drop landed on the view itself (between cells, or outside the
        // grid) rather than on a specific SessionCell - SessionCell handles
        // its own itemDropped for the precise (track, scene) target; this
        // top-level override exists only so isInterestedInDragSource makes
        // the view a valid drop target at all, per JUCE's requirement that a
        // parent claim interest before a child's own drop target is reached.
        juce::ignoreUnused (details);
    }

    void SessionView::handleClipDrop (const SourceDetails&, TrackId trackId, SceneId sceneId)
    {
        if (ctx.project == nullptr || ui.dragPayload == nullptr)
            return;

        auto* track = trackById (trackId);
        if (track == nullptr)
            return;

        const auto& dragged = *ui.dragPayload;

        performProjectEdit (project(), TRANS ("Add clip to session"), [this, trackId, sceneId, dragged]
        {
            SessionClip clip;
            clip.kind        = SessionClip::Kind::midi;
            clip.name        = dragged.name;
            clip.lengthBeats = dragged.lengthBeats;
            clip.notes       = dragged.notes;

            if (auto* t = trackById (trackId))
                t->setSessionClip (sceneId, clip);
        });

        // If this cell is the one actually playing on its track right now, the
        // engine is still looping the OLD clip from its own ring-buffer copy -
        // the document write above never reaches it. Re-launch so the engine
        // adopts the replacement at the next boundary, same as clicking a
        // different filled cell on an already-playing track (see launchCell).
        const auto activeIt = activeSceneForTrack.find (trackId);

        if (activeIt != activeSceneForTrack.end() && activeIt->second == sceneId && ctx.engine != nullptr)
            if (auto* newClip = track->findSessionClip (sceneId))
                ctx.engine->launchSessionClip (trackId, *newClip);
    }
}
