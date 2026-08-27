#include "UI/SessionView.h"
#include "Core/AppContext.h"
#include "Engine/AudioEngine.h"
#include "Mixer/Mixer.h"
#include "Plugins/BasicSynth.h"

namespace ss
{

class SessionViewUnitTests final : public juce::UnitTest
{
public:
    SessionViewUnitTests() : juce::UnitTest ("SessionView", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("addScene / renameScene / deleteScene mutate the project");
        {
            AppContext ctx;   // its constructor already builds a working engine + a blank Project
            UiState ui;

            SessionView view (ctx, ui);

            const auto before = (int) ctx.project->scenes.size();

            view.addScene();
            expectEquals ((int) ctx.project->scenes.size(), before + 1);
            const auto sceneId = ctx.project->scenes.back().id;

            view.renameScene (sceneId, "Chorus");
            expectEquals (ctx.project->scenes.back().name, juce::String ("Chorus"));

            view.deleteScene (sceneId);
            expectEquals ((int) ctx.project->scenes.size(), before);
        }

        beginTest ("launchCell starts playback on the clip's track through the engine");
        {
            // Build the whole project - track, scene, clip - BEFORE handing it to
            // ctx.setProject(), which synchronously rebuilds the mixer's track
            // states once. Adding a track to an already-installed project instead
            // would only reach the mixer via Project's async change broadcast,
            // which is not guaranteed to have run by the next line in a unit test.
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::audio, "A");
            auto& scene  = project->addScene ("Verse");

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            track.setSessionClip (scene.id, clip);

            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            expect (! ctx.engine->isSessionClipActive (trackId));
            expect (! ctx.engine->isSessionClipQueued (trackId));

            view.launchCell (trackId, sceneId);

            expect (ctx.engine->isSessionClipQueued (trackId) || ctx.engine->isSessionClipActive (trackId));
        }

        beginTest ("launchCell on an empty cell is a no-op");
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::audio, "A");
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            view.launchCell (trackId, sceneId);

            expect (! ctx.engine->isSessionClipQueued (trackId));
            expect (! ctx.engine->isSessionClipActive (trackId));
        }

        beginTest ("toggleSceneRow launches every filled cell in that row");
        {
            auto project = std::make_unique<Project>();
            auto& trackA = project->addTrack (TrackType::audio, "A");
            auto& trackB = project->addTrack (TrackType::audio, "B");
            auto& scene  = project->addScene ("Verse");

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            trackA.setSessionClip (scene.id, clip);
            trackB.setSessionClip (scene.id, clip);

            const auto trackAId = trackA.getId();
            const auto trackBId = trackB.getId();
            const auto sceneId  = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            view.toggleSceneRow (sceneId);

            expect (ctx.engine->isSessionClipQueued (trackAId) || ctx.engine->isSessionClipActive (trackAId));
            expect (ctx.engine->isSessionClipQueued (trackBId) || ctx.engine->isSessionClipActive (trackBId));
        }

        beginTest ("launchCell on a DIFFERENT scene's cell of an already-playing track launches/replaces, not stops");
        {
            // Regression for: isSessionClipActive/isSessionClipQueued only know "something is
            // running on this track", not which scene it came from - launchCell must not
            // mistake "switch to a different scene's clip on this track" for "stop the cell
            // I just clicked".
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::audio, "A");

            // Project::scenes is a std::vector<Scene> (stored by value, unlike
            // Track's own vector-of-unique_ptr) - addScene()'s returned reference
            // is only good until the NEXT addScene() call, which can reallocate
            // the vector and invalidate it. Capture each scene's id immediately
            // rather than holding the reference across the second addScene() call
            // below; the flaky "scene A's clip should be queued/active" failure
            // this test used to produce was exactly that - a dangling `sceneA`
            // read back a garbage SceneId (a stale/reused heap value, not a small
            // counter), so track.setSessionClip(sceneA.id, ...) silently filed the
            // clip under the wrong key and findSessionClip() came back null,
            // meaning launchCell() never even reached the engine.
            const auto sceneAId = project->addScene ("Verse").id;
            const auto sceneBId = project->addScene ("Chorus").id;

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            track.setSessionClip (sceneAId, clip);
            track.setSessionClip (sceneBId, clip);

            const auto trackId = track.getId();

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);

            view.launchCell (trackId, sceneAId);
            expect (ctx.engine->isSessionClipQueued (trackId) || ctx.engine->isSessionClipActive (trackId),
                    "scene A's clip should be queued/active after the first click");

            view.launchCell (trackId, sceneBId);
            expect (ctx.engine->isSessionClipQueued (trackId) || ctx.engine->isSessionClipActive (trackId),
                    "clicking a DIFFERENT filled cell on the same track must launch/replace, "
                    "never stop - the track must still be queued/active afterwards");
        }

        beginTest ("dropping a dragged MIDI clip writes a new SessionClip into the target cell");
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::midi, "Lead");
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            ui.dragPayload = std::make_shared<MidiClip> ();
            ui.dragPayload->name        = "Dragged riff";
            ui.dragPayload->lengthBeats = 6.0;
            ui.dragPayload->notes.push_back ({ 60, 0.0, 1.0, 100, 1, 1.0f });

            SessionView view (ctx, ui);

            juce::DragAndDropTarget::SourceDetails details (juce::var ("ss.timelineClip"), nullptr, {});
            view.handleClipDrop (details, trackId, sceneId);

            auto* clip = ctx.project->findTrack (trackId)->findSessionClip (sceneId);
            expect (clip != nullptr);
            if (clip != nullptr)
            {
                expect (clip->kind == SessionClip::Kind::midi);
                expectEquals (clip->name, juce::String ("Dragged riff"));
                expectWithinAbsoluteError (clip->lengthBeats, 6.0, 1.0e-9);
                expectEquals ((int) clip->notes.size(), 1);
            }
        }

        beginTest ("handleClipDrop ignores a drag with no payload");
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::midi, "Lead");
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;   // no dragPayload set
            SessionView view (ctx, ui);

            juce::DragAndDropTarget::SourceDetails details (juce::var ("ss.timelineClip"), nullptr, {});
            view.handleClipDrop (details, trackId, sceneId);

            expect (ctx.project->findTrack (trackId)->findSessionClip (sceneId) == nullptr);
        }

        /*  Final-review Critical fix: GenerateView::sendCandidatesToSession adds
            scenes+tracks through performProjectEdit (whose sendChangeMessage() is
            ASYNCHRONOUS - it does not rebuild anything synchronously) and then
            immediately calls ui.goTo(session). The first time the Session tab is
            shown, MainComponent::showView's setBounds() synchronously calls
            resized() on a SessionView whose `cells` has not been rebuilt for the
            new scenes/tracks yet. Before the fix, resized() read numScenes/
            numTracks LIVE from project() and indexed `cells` with them - a null-
            pointer crash (OwnedArray::operator[] returns nullptr out of range)
            once cells is smaller than those live counts. This reproduces exactly
            that mismatch without needing GenerateView or MainComponent: grow the
            live project directly (addTrack()/addScene() only call markDirty(),
            never sendChangeMessage(), so this never reaches rebuildGrid() at all -
            precisely the window between performProjectEdit's synchronous mutation
            and its async broadcast actually being processed), then force a
            resized() at the new, larger size. */
        beginTest ("resized() does not crash when cells lags behind a live project that grew without a rebuild");
        {
            auto project = std::make_unique<Project>();
            project->addTrack (TrackType::audio, "A");
            project->addScene ("Verse");

            AppContext ctx;
            ctx.setProject (std::move (project));

            UiState ui;
            SessionView view (ctx, ui);   // rebuildGrid() in the ctor captured 1 track x 1 scene

            view.setBounds (0, 0, 400, 300);   // sane baseline at the original size

            ctx.project->addTrack (TrackType::midi, "B");
            ctx.project->addTrack (TrackType::midi, "C");
            ctx.project->addScene ("Chorus");
            ctx.project->addScene ("Bridge");

            // Must not crash: resized() must never index further into `cells`
            // than what rebuildGrid() actually built it for.
            view.setBounds (0, 0, 800, 600);
            expect (true, "resized() survived a live project outgrowing the cached grid");

            // A real rebuild (whatever eventually delivers the async broadcast, or
            // now also attachToProject()) catches the grid up with no crash either.
            view.changeListenerCallback (nullptr);
            view.setBounds (0, 0, 800, 600);
            expect (true, "resized() is still fine once rebuildGrid() has caught up");
        }

        /*  Final-review Important fix: the identical "document says one thing,
            the engine's own ring-buffer copy says another" bug that motivated
            launchCell's scene-scoped fix, a third time - deleting a scene (or
            undoing "Add clip to session") while its clip is playing removes it
            from the document, but Mixer keeps its own independent copy and
            loops it regardless. launchCell can no longer even reach the cell
            (findSessionClip comes back null), so without rebuildGrid()'s
            reconciliation pass the loop is unstoppable short of restarting the
            app. Mixer::process() is driven directly (same technique as
            MixerTests.cpp) so this test does not depend on a real audio
            device's callback timing. */
        beginTest ("deleting a scene while its clip is playing stops the engine and forgets it");
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::audio, "A");
            auto& scene  = project->addScene ("Verse");

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            track.setSessionClip (scene.id, clip);

            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));
            ctx.engine->getDeviceManager().closeAudioDevice();   // no real callback thread racing our own process() calls below

            auto& mixer = ctx.engine->getMixer();
            mixer.prepare (48000.0, 512, 2);   // force a known-ready state deterministically

            UiState ui;
            SessionView view (ctx, ui);

            view.launchCell (trackId, sceneId);
            expect (ctx.engine->isSessionClipQueued (trackId));

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            // A huge sessionPositionSamples guarantees crossing whatever launch
            // boundary SessionClock computed, whether or not it happens to have
            // been advancing for real in the background.
            mixer.process (out, 0, midi, noInput, true, 100000000);
            expect (ctx.engine->isSessionClipActive (trackId),
                    "sanity: the clip should be genuinely active before deleting its scene");

            // Mixer keeps its own independent ring-buffer copy of the clip and
            // will keep looping it regardless of what the document says next -
            // deleting the scene from the document alone does nothing to stop it.
            view.deleteScene (sceneId);

            // Deletion goes through performProjectEdit, whose sendChangeMessage()
            // is asynchronous - changeListenerCallback() (rebuildGrid()'s only
            // other caller besides attachToProject()) is the real entry point an
            // async broadcast would eventually reach; calling it directly is this
            // project's established way of driving that path synchronously.
            view.changeListenerCallback (nullptr);

            mixer.process (out, 512, midi, noInput, true, 200000000);   // adopts the reconciliation's stop

            expect (! ctx.engine->isSessionClipActive (trackId),
                    "the reconciliation pass must stop a clip whose scene no longer exists");
            expect (! ctx.engine->isSessionClipQueued (trackId),
                    "the reconciliation pass must stop a clip whose scene no longer exists");
        }

        /*  Minor bundle, same root cause: dropping a replacement clip onto the
            cell that is ACTUALLY playing must make the engine pick up the new
            clip, not just the document - otherwise the grid shows the new
            clip's name while the engine keeps looping the OLD one. Proven the
            same way MixerTests.cpp proves session audio behaviour: BasicSynth
            makes a still-sounding note observable as audio that does not stop
            when it should. */
        beginTest ("dropping a replacement clip onto the actively-playing cell re-launches the engine with it");
        {
            auto project = std::make_unique<Project>();
            auto& track  = project->addTrack (TrackType::midi, "Lead");
            track.plugins.push_back ({ BasicSynth::identifier, "Basic Synth", false, true, {} });
            auto& scene  = project->addScene ("Verse");
            const auto trackId = track.getId();
            const auto sceneId = scene.id;

            AppContext ctx;
            ctx.setProject (std::move (project));
            ctx.engine->getDeviceManager().closeAudioDevice();   // no real callback thread racing our own process() calls below

            auto& mixer = ctx.engine->getMixer();
            mixer.prepare (48000.0, 512, 2);   // force a known-ready state deterministically

            UiState ui;
            ui.dragPayload = std::make_shared<MidiClip>();
            ui.dragPayload->name        = "Sustained note";
            ui.dragPayload->lengthBeats = 8.0;
            ui.dragPayload->notes.push_back ({ 60, 0.0, 8.0, 100, 1, 1.0f });   // fills the whole clip

            SessionView view (ctx, ui);

            juce::DragAndDropTarget::SourceDetails details (juce::var ("ss.timelineClip"), nullptr, {});
            view.handleClipDrop (details, trackId, sceneId);   // creates the first (sustained-note) clip

            view.launchCell (trackId, sceneId);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 100000000);   // adopts the launch; note-on
            expect (out.getMagnitude (0, 0, 512) > 0.01f, "sanity: the first clip's note should be sounding");

            // Drop a SILENT replacement onto the same, actively-playing cell -
            // exactly like dragging a different candidate onto a cell that is
            // already running.
            ui.dragPayload->name = "Silence";
            ui.dragPayload->notes.clear();
            view.handleClipDrop (details, trackId, sceneId);

            out.clear();
            mixer.process (out, 512, midi, noInput, true, 200000000);   // crosses the re-launch boundary

            // BasicSynth has a release tail (~0.25s default) - a re-launched,
            // silent clip's own all-notes-off takes that long to actually decay
            // to silence, same reasoning as MixerTests.cpp's stop/replace tests.
            float lastMagnitude = 0.0f;

            for (int i = 0; i < 60; ++i)
            {
                out.clear();
                const auto sessionPos = 200000512 + (juce::int64) i * 512;
                mixer.process (out, sessionPos, midi, noInput, true, sessionPos);
                lastMagnitude = out.getMagnitude (0, 0, out.getNumSamples());
            }

            expect (lastMagnitude < 1.0e-3f,
                    "handleClipDrop must re-launch the engine with the replacement clip when the "
                    "dropped cell is the one actively playing - otherwise the engine keeps looping "
                    "the OLD clip's audio even though the document now shows the new one");
        }
    }
};

static SessionViewUnitTests sessionViewUnitTests;

}
