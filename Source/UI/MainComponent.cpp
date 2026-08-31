#include "UI/MainComponent.h"
#include "Core/Localisation.h"
#include "Core/Settings.h"
#include "Engine/AudioEngine.h"
#include "Extensions/FormatExtensionManager.h"
#include "Extensions/FormatExtensionRunner.h"
#include "IO/DawProject.h"
#include "IO/FileIO.h"
#include "Plugins/PluginManager.h"
#include "UI/ExtensionHelpDialog.h"
#include "UI/GenerateView.h"
#include "UI/MixerView.h"
#include "UI/NotationView.h"
#include "UI/PianoRollView.h"
#include "UI/PreferencesDialog.h"
#include "UI/SessionView.h"
#include "UI/SidePanels.h"
#include "UI/TimelineView.h"
#include "UI/TranscribeView.h"
#include "UI/TransportBar.h"
#include "UI/UiSupport.h"
#include "UI/WaveformEditorView.h"
#include "UI/Dock/DockContainer.h"
#include "UI/Dock/DockLayout.h"
#include "UI/Dock/FloatingDockWindow.h"
#include <algorithm>

namespace ss
{
    namespace
    {
        constexpr int menuBarHeight  = 24;
        constexpr int toolbarHeight  = 36;
        constexpr int transportHeight = 56;
        constexpr int resizerWidth   = 5;

        // Hand-rolled PopupMenu ids for the dynamically-discovered format
        // extensions (their count varies at runtime, so they can't live in the
        // fixed CommandIDs enum). 1000-wide bands comfortably outlive any
        // realistic number of installed extensions and never collide with the
        // existing hand-rolled ids (20001-20003) used elsewhere in this file.
        constexpr int importExtensionMenuIdBase = 21000;
        constexpr int exportExtensionMenuIdBase = 22000;

        /** Screens the spec puts in a later phase (9.10 session view, 9.11
            modular patching).  MainComponent::View has entries for them, so they
            get an honest panel rather than a broken one. */
        class PlaceholderView final : public juce::Component
        {
        public:
            PlaceholderView (juce::String titleToUse, juce::String bodyToUse, juce::String phaseToUse)
                : title (std::move (titleToUse)), body (std::move (bodyToUse)), phase (std::move (phaseToUse)) {}

            void paint (juce::Graphics& g) override
            {
                const auto& p = palette();
                g.fillAll (p.windowBg);

                auto area = getLocalBounds().withSizeKeepingCentre (juce::jmin (520, getWidth() - 40),
                                                                    juce::jmin (200, getHeight() - 40));
                g.setColour (p.panelBg);
                g.fillRoundedRectangle (area.toFloat(), 6.0f);
                g.setColour (p.outline);
                g.drawRoundedRectangle (area.toFloat(), 6.0f, 1.0f);

                area = area.reduced (24, 20);
                g.setColour (p.textBright);
                g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
                g.drawText (title, area.removeFromTop (28), juce::Justification::centredLeft, false);

                g.setColour (p.warning);
                g.setFont (juce::Font (juce::FontOptions (12.0f)));
                g.drawText (phase, area.removeFromTop (20), juce::Justification::centredLeft, false);

                area.removeFromTop (8);
                g.setColour (p.textDim);
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawFittedText (body, area, juce::Justification::topLeft, 6);
            }

        private:
            juce::String title, body, phase;
        };

        std::unique_ptr<Project> createDefaultProject (Settings& settings)
        {
            auto project = std::make_unique<Project>();
            project->sampleRate = settings.getDefaultSampleRate();
            project->bitDepth   = settings.getDefaultBitDepth();
            project->tempo.setEvents ({ { 0.0, settings.getDefaultBpm() } });
            project->tempo.setTimeSignatures ({ { 0.0,
                                                  settings.raw().getIntValue ("project.defaultTimeSigNum", 4),
                                                  settings.raw().getIntValue ("project.defaultTimeSigDen", 4) } });

            switch (settings.raw().getIntValue ("project.defaultTemplate", 1))
            {
                case 2:
                    project->addTrack (TrackType::audio, TRANS ("Vocal"));
                    project->addTrack (TrackType::audio, TRANS ("Guitar"));
                    break;

                case 3:
                    project->addTrack (TrackType::midi,  TRANS ("Drums"));
                    project->addTrack (TrackType::midi,  TRANS ("Bass"));
                    project->addTrack (TrackType::midi,  TRANS ("Keys"));
                    project->addTrack (TrackType::audio, TRANS ("Guitar"));
                    project->addTrack (TrackType::audio, TRANS ("Vocal"));
                    break;

                default:
                    break;
            }

            project->clearDirty();
            return project;
        }
    }

    //==========================================================================
    struct MainComponent::Impl : private juce::Timer
    {
        Impl (MainComponent& o, AppContext& c) : owner (o), ctx (c) {}

        /** Runs after MainComponent::impl has been assigned: registering commands
            calls straight back into MainComponent, which dereferences impl. */
        void initialise()
        {
            auto& settings = *ctx.settings;

            // Language and theme first: everything built below runs its labels
            // through TRANS() exactly once, at construction.
            currentLanguage = settings.getLanguage();
            setUiLanguage (currentLanguage);
            lookAndFeel.setTheme (settings.getTheme());
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            juce::Desktop::getInstance().setGlobalScaleFactor (settings.getUiScale());

            commands.registerAllCommandsForTarget (&owner);
            commands.setFirstCommandTarget (&owner);
            if (auto xml = settings.getKeyMappings())
                commands.getKeyMappings()->restoreFromXml (*xml);

            owner.setApplicationCommandManagerToWatch (&commands);
            owner.setWantsKeyboardFocus (true);
            owner.addKeyListener (commands.getKeyMappings());

            uiState.requestView   = [this] (View v) { showView (v); };
            uiState.invokeCommand = [this] (juce::CommandID id) { commands.invokeDirectly (id, false); };

            browserWidth = settings.raw().getIntValue ("ui.browserWidth", 250);
            aiWidth      = settings.raw().getIntValue ("ui.aiPanelWidth", 280);

            buildUi();
            showView (View::timeline);

            // Keeps the toolbar's enabled/ticked state honest while the transport
            // runs; commands have no other way to know the playhead moved.
            startTimerHz (5);
        }

        ~Impl() override
        {
            stopTimer();
            saveLayout();
            owner.removeKeyListener (commands.getKeyMappings());
            owner.setApplicationCommandManagerToWatch (nullptr);
            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        }

        //----------------------------------------------------------------------
        void saveLayout()
        {
            if (ctx.settings == nullptr)
                return;

            ctx.settings->raw().setValue ("ui.browserWidth", browserWidth);
            ctx.settings->raw().setValue ("ui.aiPanelWidth", aiWidth);
            ctx.settings->raw().setValue ("ui.browserVisible", browserVisible);
            ctx.settings->raw().setValue ("ui.aiPanelVisible", aiVisible);

            // Persist "what the dock layout looked like when we closed" -
            // restored as DockLayout::lastSessionLayoutName the next time
            // buildUi() runs.
            if (workspace != nullptr)
                ctx.settings->setDockLayout (DockLayout::lastSessionLayoutName, workspace->toVar());

            ctx.settings->flush();
        }

        void buildUi()
        {
            auto& settings = *ctx.settings;
            browserVisible = settings.raw().getBoolValue ("ui.browserVisible", true);
            aiVisible      = settings.raw().getBoolValue ("ui.aiPanelVisible", true);

            menuBar = std::make_unique<juce::MenuBarComponent> (&owner);
            owner.addAndMakeVisible (*menuBar);

            buildToolbar();

            browser = std::make_unique<BrowserPanel> (ctx, uiState);
            owner.addAndMakeVisible (*browser);

            aiPanel = std::make_unique<AiPanel> (ctx, uiState);
            owner.addAndMakeVisible (*aiPanel);

            transport = std::make_unique<TransportBar> (ctx, uiState, commands);
            owner.addAndMakeVisible (*transport);

            timeline   = std::make_unique<TimelineView> (ctx, uiState);
            pianoRoll  = std::make_unique<PianoRollView> (ctx, uiState);
            mixer      = std::make_unique<MixerView> (ctx, uiState);
            transcribe = std::make_unique<TranscribeView> (ctx, uiState);
            generate   = std::make_unique<GenerateView> (ctx, uiState);
            notation   = std::make_unique<NotationView> (ctx, uiState);

            session = std::make_unique<SessionView> (ctx, uiState);

            modular = std::make_unique<PlaceholderView> (
                TRANS ("Modular patching"),
                TRANS ("An infinite canvas of module nodes wired output to input, saved as presets that can be "
                       "dropped into any track as an instrument or an effect."),
                TRANS ("Planned for Phase 3"));

            const std::map<juce::String, juce::Component*> panelsById {
                { "timeline", timeline.get() }, { "pianoRoll", pianoRoll.get() }, { "mixer", mixer.get() },
                { "transcribe", transcribe.get() }, { "generate", generate.get() }, { "notation", notation.get() },
                { "session", session.get() }, { "modular", modular.get() }
            };

            const std::map<juce::String, juce::String> displayNamesById {
                { "timeline", TRANS ("Timeline") }, { "pianoRoll", TRANS ("Piano Roll") },
                { "mixer", TRANS ("Mixer") }, { "transcribe", TRANS ("Transcribe") },
                { "generate", TRANS ("Generate") }, { "notation", TRANS ("Notation") },
                { "session", TRANS ("Session") }, { "modular", TRANS ("Modular") }
            };

            const auto layoutNameToUse = DockLayout::resolveStartupLayoutName (ctx.settings->getStartupDockLayoutName());
            const auto savedLayout = pendingLayoutOverride.isObject()
                                          ? pendingLayoutOverride
                                          : ctx.settings->getDockLayout (layoutNameToUse);
            auto restoredRoot = savedLayout.isObject()
                                     ? DockLayout::restore (savedLayout, panelsById, displayNamesById)
                                     : nullptr;

            if (restoredRoot == nullptr)
                restoredRoot = DockLayout::restore (DockLayout::defaultLayout (displayNamesById),
                                                    panelsById, displayNamesById);

            workspace = std::make_unique<DockContainer> (std::move (restoredRoot));
            owner.addAndMakeVisible (*workspace);

            workspace->onPanelDraggedOutside = [this] (DockPanel panel)
            {
                auto newGroup = std::make_unique<DockTabGroup>();
                newGroup->addPanel (panel);

                auto window = std::make_unique<FloatingDockWindow> (std::move (newGroup));
                window->setBounds (juce::Rectangle<int> (100, 100, 500, 400));
                window->onClosing = [this] (FloatingDockWindow* closing)
                {
                    // The erase below destroys the window AND the DockContainer
                    // tree inside it. The panels' content components survive
                    // (nothing in Dock ever owns them) but would be orphaned for
                    // the rest of the session: detached from every dock tree,
                    // invisible, and unreachable by showView/saveLayout. Rescue
                    // them into the main workspace first. This is a safety valve,
                    // not persistence - floating windows themselves still don't
                    // survive a relaunch, which stays a documented follow-up.
                    if (workspace != nullptr)
                    {
                        std::function<DockTabGroup*(DockNode&)> findAnyGroup = [&] (DockNode& node) -> DockTabGroup*
                        {
                            if (auto* group = dynamic_cast<DockTabGroup*> (&node))
                                return group;

                            if (auto* split = dynamic_cast<DockSplit*> (&node))
                            {
                                if (auto* found = findAnyGroup (split->getFirst()))
                                    return found;
                                return findAnyGroup (split->getSecond());
                            }

                            return nullptr;
                        };

                        if (auto* home = findAnyGroup (workspace->getRoot()))
                            for (auto& rescued : closing->getDockContainer().extractAllPanels())
                                home->addPanel (rescued);
                    }

                    // If workspace is already gone (shutdown ordering) there is
                    // nowhere to put a rescued panel and it stays orphaned - the
                    // whole UI is being torn down anyway.
                    floatingWindows.erase (std::remove_if (floatingWindows.begin(), floatingWindows.end(),
                                                            [closing] (auto& w) { return w.get() == closing; }),
                                           floatingWindows.end());
                };
                window->setVisible (true);
                floatingWindows.push_back (std::move (window));
            };

            leftBar  = std::make_unique<juce::StretchableLayoutResizerBar> (&layout, 1, true);
            rightBar = std::make_unique<juce::StretchableLayoutResizerBar> (&layout, 3, true);
            owner.addAndMakeVisible (*leftBar);
            owner.addAndMakeVisible (*rightBar);

            owner.addChildComponent (task);

            updatePanelVisibility();
            owner.resized();
        }

        void rebuildUi()
        {
            // A language change is the only thing that needs this: every label was
            // translated once at construction.  UiState survives, so the selection,
            // grid and key all carry over.
            const auto view = currentView;
            const auto layoutToRestore = workspace != nullptr ? workspace->toVar() : juce::var();

            owner.removeAllChildren();
            timeline.reset(); pianoRoll.reset(); mixer.reset(); transcribe.reset();
            generate.reset(); notation.reset(); session.reset(); modular.reset();
            workspace.reset();
            floatingWindows.clear();
            browser.reset(); aiPanel.reset(); transport.reset();
            leftBar.reset(); rightBar.reset(); menuBar.reset();
            toolbarButtons.clear();
            toolbarLayout.clear();

            pendingLayoutOverride = layoutToRestore; // buildUi() reads this instead of Settings, see Step 1
            buildUi();
            pendingLayoutOverride = juce::var();

            showView (view);
            owner.repaint();
        }

        std::vector<ProjectView*> projectViews() const
        {
            std::vector<ProjectView*> views;
            for (ProjectView* v : { (ProjectView*) timeline.get(), (ProjectView*) pianoRoll.get(),
                                    (ProjectView*) mixer.get(), (ProjectView*) transcribe.get(),
                                    (ProjectView*) generate.get(), (ProjectView*) notation.get(),
                                    (ProjectView*) session.get(),
                                    (ProjectView*) browser.get(), (ProjectView*) aiPanel.get(),
                                    (ProjectView*) transport.get() })
                if (v != nullptr)
                    views.push_back (v);

            return views;
        }

        //----------------------------------------------------------------------
        void buildToolbar()
        {
            auto addButton = [this] (const juce::String& text, juce::CommandID id, int width,
                                     const juce::String& tooltip, bool separatorAfter = false)
            {
                auto* b = toolbarButtons.add (new juce::TextButton (text));
                b->setCommandToTrigger (&commands, id, true);
                b->setTooltip (tooltip);
                b->setConnectedEdges (0);
                owner.addAndMakeVisible (b);
                toolbarLayout.push_back ({ b, width, separatorAfter });
            };

            addButton (TRANS ("New"), CommandIDs::fileNew, 56, TRANS ("New project"));
            addButton (TRANS ("Open"), CommandIDs::fileOpen, 60, TRANS ("Open a project"));
            addButton (TRANS ("Save"), CommandIDs::fileSave, 60, TRANS ("Save the project"), true);

            addButton (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xb6")),
                       juce::StandardApplicationCommandIDs::undo, 40, TRANS ("Undo"));
            addButton (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xb7")),
                       juce::StandardApplicationCommandIDs::redo, 40, TRANS ("Redo"));
            addButton (TRANS ("Split"), CommandIDs::splitClipAtPlayhead, 60,
                       TRANS ("Split the selected clips at the playhead"), true);

            addButton (TRANS ("Timeline"), CommandIDs::viewTimeline, 84, TRANS ("Arrangement"));
            addButton (TRANS ("Piano Roll"), CommandIDs::viewPianoRoll, 92, TRANS ("Note editor"));
            addButton (TRANS ("Mixer"), CommandIDs::viewMixer, 68, TRANS ("Mixer"));
            addButton (TRANS ("Transcribe"), CommandIDs::viewTranscribe, 96, TRANS ("Audio to MIDI"));
            addButton (TRANS ("Generate"), CommandIDs::viewGenerate, 86, TRANS ("MIDI generation"));
            addButton (TRANS ("Score"), CommandIDs::viewNotation, 68, TRANS ("Notation"), true);

            snapButton.setButtonText (TRANS ("Snap"));
            snapButton.setToggleState (uiState.snapEnabled, juce::dontSendNotification);
            snapButton.onClick = [this]
            {
                uiState.snapEnabled = snapButton.getToggleState();
                uiState.sendChangeMessage();
            };
            owner.addAndMakeVisible (snapButton);

            fillQuantiseComboBox (gridBox, uiState.grid);
            gridBox.setTooltip (TRANS ("Grid"));
            gridBox.onChange = [this]
            {
                uiState.grid = quantiseFromComboBox (gridBox);
                uiState.sendChangeMessage();
            };
            owner.addAndMakeVisible (gridBox);

            addButton ("-", CommandIDs::zoomOut, 34, TRANS ("Zoom out"));
            addButton ("+", CommandIDs::zoomIn, 34, TRANS ("Zoom in"), true);
            addButton (TRANS ("Preferences"), CommandIDs::showPreferences, 108, TRANS ("Preferences"));
        }

        //----------------------------------------------------------------------
        void showView (View view)
        {
            currentView = view;

            if (workspace == nullptr)
                return;

            const auto id = idForView (view);

            std::function<DockTabGroup*(DockNode&)> findGroupContaining = [&] (DockNode& node) -> DockTabGroup*
            {
                if (auto* group = dynamic_cast<DockTabGroup*> (&node))
                    return group->indexOfPanel (id) >= 0 ? group : nullptr;

                if (auto* split = dynamic_cast<DockSplit*> (&node))
                {
                    if (auto* found = findGroupContaining (split->getFirst()))
                        return found;
                    return findGroupContaining (split->getSecond());
                }

                return nullptr;
            };

            if (auto* group = findGroupContaining (workspace->getRoot()))
                group->setActivePanel (group->indexOfPanel (id));
            else
                for (auto& floating : floatingWindows)
                    if (auto* floatingGroup = findGroupContaining (floating->getDockContainer().getRoot()))
                    {
                        floatingGroup->setActivePanel (floatingGroup->indexOfPanel (id));
                        floating->toFront (true);
                        break;
                    }

            // The timeline's "Transcribe this range" hands over here.
            if (view == View::transcribe && transcribe != nullptr)
                transcribe->consumePendingRequest();

            commands.commandStatusChanged();
        }

        static juce::String idForView (View view)
        {
            switch (view)
            {
                case View::timeline:   return "timeline";
                case View::pianoRoll:  return "pianoRoll";
                case View::mixer:      return "mixer";
                case View::transcribe: return "transcribe";
                case View::generate:   return "generate";
                case View::notation:   return "notation";
                case View::session:    return "session";
                case View::modular:    return "modular";
            }

            return {};
        }

        void updatePanelVisibility()
        {
            if (browser != nullptr) browser->setVisible (browserVisible);
            if (aiPanel != nullptr) aiPanel->setVisible (aiVisible);
            if (leftBar != nullptr) leftBar->setVisible (browserVisible);
            if (rightBar != nullptr) rightBar->setVisible (aiVisible);
        }

        void resized()
        {
            auto area = owner.getLocalBounds();
            if (area.isEmpty())
                return;

            // Only draw our own menu bar when the window has not taken it (macOS
            // main menu, or DocumentWindow::setMenuBar from Main.cpp).
            bool hostOwnsMenuBar = false;
           #if JUCE_MAC
            hostOwnsMenuBar = juce::MenuBarModel::getMacMainMenu() != nullptr;
           #else
            if (auto* window = owner.findParentComponentOfClass<juce::DocumentWindow>())
                hostOwnsMenuBar = window->getMenuBarComponent() != nullptr;
           #endif

            if (menuBar != nullptr)
            {
                menuBar->setVisible (! hostOwnsMenuBar);
                if (! hostOwnsMenuBar)
                    menuBar->setBounds (area.removeFromTop (menuBarHeight));
            }

            layOutToolbar (area.removeFromTop (toolbarHeight));

            if (transport != nullptr)
                transport->setBounds (area.removeFromBottom (transportHeight));

            // Three resizable panes (spec 9.1), sizes persisted in Settings.
            layout.setItemLayout (0, browserVisible ? 150.0 : 0.0, browserVisible ? 520.0 : 0.0,
                                  browserVisible ? (double) browserWidth : 0.0);
            layout.setItemLayout (1, browserVisible ? resizerWidth : 0, browserVisible ? resizerWidth : 0,
                                  browserVisible ? resizerWidth : 0);
            layout.setItemLayout (2, 320.0, -1.0, -1.0);
            layout.setItemLayout (3, aiVisible ? resizerWidth : 0, aiVisible ? resizerWidth : 0,
                                  aiVisible ? resizerWidth : 0);
            layout.setItemLayout (4, aiVisible ? 180.0 : 0.0, aiVisible ? 560.0 : 0.0,
                                  aiVisible ? (double) aiWidth : 0.0);

            juce::Component* comps[] = { browser.get(), leftBar.get(), workspace.get(),
                                         rightBar.get(), aiPanel.get() };
            layout.layOutComponents (comps, 5, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                     false, true);

            if (browserVisible) browserWidth = juce::jmax (0, (int) layout.getItemCurrentAbsoluteSize (0));
            if (aiVisible)      aiWidth      = juce::jmax (0, (int) layout.getItemCurrentAbsoluteSize (4));

            task.setBounds (owner.getLocalBounds().withSizeKeepingCentre (460, 72));
        }

        void layOutToolbar (juce::Rectangle<int> strip)
        {
            auto area = strip.reduced (6, 4);

            for (const auto& entry : toolbarLayout)
            {
                if (entry.button == nullptr) continue;
                entry.button->setBounds (area.removeFromLeft (entry.width).reduced (1, 0));

                if (entry.separatorAfter)
                    area.removeFromLeft (12);

                // The grid controls sit between the view switcher and the zoom
                // buttons; place them once the view buttons are laid out.
                if (entry.button->getCommandID() == CommandIDs::viewNotation)
                {
                    snapButton.setBounds (area.removeFromLeft (62));
                    gridBox.setBounds (area.removeFromLeft (78).reduced (2, 0));
                    area.removeFromLeft (10);
                }
            }
        }

        //----------------------------------------------------------------------
        void timerCallback() override
        {
            const bool playing = ctx.engine != nullptr && ctx.engine->getTransport().isPlaying();
            const bool recording = ctx.engine != nullptr && ctx.engine->getTransport().isRecording();
            const bool dirty = ctx.project != nullptr && ctx.project->hasUnsavedChanges();

            if (playing != lastPlaying || recording != lastRecording || dirty != lastDirty)
            {
                lastPlaying = playing;
                lastRecording = recording;
                lastDirty = dirty;
                commands.commandStatusChanged();
            }

            // The grid and snap live in UiState, so any view can change them;
            // the toolbar just follows rather than owning them.
            if (snapButton.getToggleState() != uiState.snapEnabled)
                snapButton.setToggleState (uiState.snapEnabled, juce::dontSendNotification);

            const auto& grids = quantiseMenuValues();
            for (int i = 0; i < (int) grids.size(); ++i)
                if (grids[(size_t) i] == uiState.grid && gridBox.getSelectedId() != i + 1)
                    gridBox.setSelectedId (i + 1, juce::dontSendNotification);

            handleAutoSave();
        }

        void handleAutoSave()
        {
            if (ctx.settings == nullptr || ctx.project == nullptr)
                return;

            const int interval = ctx.settings->getAutoSaveIntervalSeconds();
            if (interval <= 0 || ! ctx.project->hasUnsavedChanges())
                return;

            const auto now = juce::Time::getCurrentTime();
            if ((now - lastAutoSave).inSeconds() < interval)
                return;

            lastAutoSave = now;

            auto folder = ctx.settings->getBackupFolder();
            folder.createDirectory();

            const auto stem = ctx.project->getFile().existsAsFile()
                                ? ctx.project->getFile().getFileNameWithoutExtension()
                                : juce::String ("Untitled");

            const auto destination = folder.getChildFile (stem + "-autosave-"
                                                            + now.formatted ("%Y%m%d-%H%M%S") + ".ssproj");

            // Deliberately NOT Project::saveTo(): a normal save is documented to
            // set the document's file and clear its dirty flag, which would quietly
            // repoint the user's next Ctrl+S at a backup.  Serialising toVar()
            // straight to disk leaves the live document untouched.
            // ponytail: assumes .ssproj is the JSON of toVar(); if the on-disk
            // format ever gains a wrapper, route this through an IO helper instead.
            destination.replaceWithText (juce::JSON::toString (ctx.project->toVar()));

            // Keep only the newest N generations of this project's autosaves.
            const int generations = juce::jmax (1, ctx.settings->getBackupGenerations());
            juce::Array<juce::File> existing;
            folder.findChildFiles (existing, juce::File::findFiles, false, stem + "-autosave-*.ssproj");
            if (existing.size() > generations)
            {
                existing.sort();
                for (int i = 0; i < existing.size() - generations; ++i)
                    existing.getReference (i).deleteFile();
            }
        }

        //----------------------------------------------------------------------
        // Document handling
        //----------------------------------------------------------------------
        void replaceProject (std::unique_ptr<Project> newProject)
        {
            // Views must let go of the old document BEFORE it is destroyed.
            for (auto* view : projectViews())
                view->detachFromProject();

            ctx.setProject (std::move (newProject));

            if (ctx.project != nullptr && ctx.settings != nullptr)
                ctx.project->getUndoManager().setMaxNumberOfStoredUnits (
                    ctx.settings->getUndoHistoryLimit() * projectSnapshotActionUnitsPerStep, 1);

            for (auto* view : projectViews())
                view->attachToProject();

            uiState.select (invalidTrackId, invalidClipId, false);
            uiState.transcribeRequest = {};
            uiState.sendChangeMessage();

            if (ctx.project != nullptr)
                ctx.project->sendChangeMessage();

            commands.commandStatusChanged();
        }

        void withUnsavedChangesHandled (std::function<void (bool proceed)> next)
        {
            if (ctx.project == nullptr || ! ctx.project->hasUnsavedChanges())
            {
                next (true);
                return;
            }

            juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon,
                                                   TRANS ("Unsaved changes"),
                                                   TRANS ("Save the changes to") + " \""
                                                     + ctx.project->name + "\"?",
                                                   TRANS ("Save"), TRANS ("Discard"), TRANS ("Cancel"),
                                                   &owner,
                                                   juce::ModalCallbackFunction::create ([this, next] (int result)
            {
                if (result == 1)      saveProject ([next] (bool saved) { next (saved); });
                else if (result == 2) next (true);
                else                  next (false);
            }));
        }

        void newProject()
        {
            withUnsavedChangesHandled ([this] (bool proceed)
            {
                if (proceed)
                    replaceProject (createDefaultProject (*ctx.settings));
            });
        }

        void openProject()
        {
            withUnsavedChangesHandled ([this] (bool proceed)
            {
                if (! proceed) return;

                chooser = std::make_unique<juce::FileChooser> (TRANS ("Open project"),
                                                               ctx.settings->getProjectsFolder(), "*.ssproj");
                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [this] (const juce::FileChooser& fc)
                {
                    const auto file = fc.getResult();
                    if (! file.existsAsFile()) return;

                    auto project = std::make_unique<Project>();
                    const auto result = project->loadFrom (file);

                    if (result.failed())
                    {
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                TRANS ("Could not open the project"),
                                                                result.getErrorMessage(), TRANS ("OK"), &owner);
                        return;
                    }

                    replaceProject (std::move (project));
                });
            });
        }

        void saveProject (std::function<void (bool)> done)
        {
            if (ctx.project == nullptr)
            {
                if (done) done (false);
                return;
            }

            if (! ctx.project->getFile().getFullPathName().isEmpty())
            {
                const auto result = ctx.project->saveTo (ctx.project->getFile());
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Could not save the project"),
                                                            result.getErrorMessage(), TRANS ("OK"), &owner);
                if (done) done (result.wasOk());
                return;
            }

            saveProjectAs (std::move (done));
        }

        void saveProjectAs (std::function<void (bool)> done)
        {
            chooser = std::make_unique<juce::FileChooser> (TRANS ("Save project as"),
                                                           ctx.settings->getProjectsFolder()
                                                               .getChildFile (ctx.project != nullptr
                                                                                ? ctx.project->name
                                                                                : juce::String ("Untitled"))
                                                               .withFileExtension ("ssproj"),
                                                           "*.ssproj");

            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, done] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr)
                {
                    if (done) done (false);
                    return;
                }

                file = file.withFileExtension ("ssproj");
                const auto result = ctx.project->saveTo (file);

                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Could not save the project"),
                                                            result.getErrorMessage(), TRANS ("OK"), &owner);
                else
                    ctx.project->sendChangeMessage();

                if (done) done (result.wasOk());
            });
        }

        //----------------------------------------------------------------------
        void importFiles (bool audio)
        {
            const auto patterns = audio ? io::getSupportedAudioExtensions().joinIntoString (";")
                                        : juce::String ("*.mid;*.midi");

            chooser = std::make_unique<juce::FileChooser> (audio ? TRANS ("Import audio") : TRANS ("Import MIDI"),
                                                           juce::File(), patterns);
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::canSelectMultipleItems,
                                  [this, audio] (const juce::FileChooser& fc)
            {
                if (ctx.project == nullptr) return;

                const auto files = fc.getResults();
                if (files.isEmpty()) return;

                const double at = ctx.engine != nullptr
                                    ? juce::jmax (0.0, ctx.engine->getTransport().getPositionBeats()) : 0.0;

                performProjectEdit (*ctx.project, TRANS ("Import files"), [this, files, audio, at]
                {
                    for (const auto& file : files)
                    {
                        juce::String error;

                        if (! audio)
                        {
                            io::importMidiFile (file, *ctx.project, error);
                            continue;
                        }

                        const auto track = ctx.project->addTrack (TrackType::audio,
                                                                  file.getFileNameWithoutExtension()).getId();
                        if (ctx.engine != nullptr)
                            io::importAudioFile (file, *ctx.project, track, at,
                                                 ctx.engine->getFormatManager(), error);
                    }
                });
            });
        }

        void exportMidi()
        {
            chooser = std::make_unique<juce::FileChooser> (TRANS ("Export MIDI"),
                                                           ctx.settings->getProjectsFolder(), "*.mid");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                juce::String error;
                if (! io::exportMidiFile (file.withFileExtension ("mid"), *ctx.project, error))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), error, TRANS ("OK"), &owner);
            });
        }

        void exportMusicXml()
        {
            chooser = std::make_unique<juce::FileChooser> (TRANS ("Export MusicXML"),
                                                           ctx.settings->getProjectsFolder(), "*.musicxml;*.xml");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                if (file.getFileExtension().isEmpty())
                    file = file.withFileExtension ("musicxml");

                juce::String error;
                if (! io::exportMusicXml (file, *ctx.project, error))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), error, TRANS ("OK"), &owner);
            });
        }

        void showDawProjectWarnings (const juce::StringArray& warnings)
        {
            if (warnings.isEmpty())
                return;
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                    TRANS ("Some things could not be transferred"),
                                                    warnings.joinIntoString ("\n"), TRANS ("OK"), &owner);
        }

        void exportDawProject()
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (TRANS ("Export DAWproject"),
                                                            ctx.settings->getProjectsFolder(), "*.dawproject");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;
                file = file.withFileExtension ("dawproject");

                juce::String error;
                juce::StringArray warnings;
                if (! io::exportDawProject (file, *ctx.project, *ctx.plugins, error, warnings))
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), error, TRANS ("OK"), &owner);
                    return;
                }
                showDawProjectWarnings (warnings);
            });
        }

        void importDawProject()
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (TRANS ("Import DAWproject"),
                                                            juce::File(), "*.dawproject");
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                auto error = std::make_shared<juce::String>();
                auto warnings = std::make_shared<juce::StringArray>();
                bool ok = false;

                performProjectEdit (*ctx.project, TRANS ("Import DAWproject"), [this, file, error, warnings, &ok]
                {
                    ok = io::importDawProject (file, *ctx.project, *ctx.plugins, *error, *warnings);
                });

                if (! ok)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Import failed"), *error, TRANS ("OK"), &owner);
                else
                    showDawProjectWarnings (*warnings);
            });
        }

        void rescanFormatExtensions()
        {
            if (ctx.formatExtensions == nullptr)
                return;
            juce::StringArray warnings;   // shown in Preferences > Extensions, not here
            ctx.formatExtensions->rescan (ctx.settings->getExtensionScanPaths(), warnings);
        }

        void exportViaExtension (const FormatExtension& extension)
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (extension.name,
                                                            ctx.settings->getProjectsFolder(),
                                                            "*." + extension.fileExtension);
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, extension] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;
                file = file.withFileExtension (extension.fileExtension);

                juce::String error;
                juce::StringArray warnings;
                ExternalFormatExtensionRunner runner;
                if (! io::runFormatExtensionExport (extension, *ctx.project, *ctx.plugins, file,
                                                    runner, error, warnings))
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), error, TRANS ("OK"), &owner);
                    return;
                }
                showDawProjectWarnings (warnings);
            });
        }

        void importViaExtension (const FormatExtension& extension)
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (extension.name, juce::File(),
                                                            "*." + extension.fileExtension);
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this, extension] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                auto error = std::make_shared<juce::String>();
                auto warnings = std::make_shared<juce::StringArray>();
                bool ok = false;
                ExternalFormatExtensionRunner runner;

                performProjectEdit (*ctx.project, TRANS ("Import") + " " + extension.name,
                                    [this, file, extension, error, warnings, &ok, &runner]
                {
                    ok = io::runFormatExtensionImport (extension, *ctx.project, *ctx.plugins, file,
                                                       runner, *error, *warnings);
                });

                if (! ok)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Import failed"), *error, TRANS ("OK"), &owner);
                else
                    showDawProjectWarnings (*warnings);
            });
        }

        void exportAudio()
        {
            if (ctx.project == nullptr || ctx.engine == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (TRANS ("Export audio"),
                                                           ctx.settings->getProjectsFolder(), "*.wav");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                file = file.withFileExtension ("wav");
                const double endBeat = juce::jmax (1.0, ctx.project->endBeats());
                const int bitDepth = ctx.project->bitDepth;
                const double sampleRate = ctx.project->sampleRate;
                auto error = std::make_shared<juce::String>();

                // ponytail: mixdown only.  AudioEngine::renderToFile already takes
                // a stemPerTrack flag - expose it here when someone asks for stems.
                task.run (TRANS ("Rendering..."), [this, file, endBeat, bitDepth, sampleRate, error] (TaskPanel& t)
                {
                    const auto result = ctx.engine->renderToFile (file, 0.0, endBeat, bitDepth, sampleRate,
                                                                  false, [&t] (float p) { t.setProgress (p); });
                    if (result.failed())
                        *error = result.getErrorMessage();
                },
                [this, error]
                {
                    if (error->isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                TRANS ("Render failed"), *error,
                                                                TRANS ("OK"), &owner);
                });
            });
        }

        //----------------------------------------------------------------------
        void addTrack (TrackType type)
        {
            if (ctx.project == nullptr) return;

            auto* p = ctx.project.get();
            performProjectEdit (*p, type == TrackType::audio ? TRANS ("Add audio track")
                                                             : TRANS ("Add MIDI track"),
                                [p, type]
            {
                p->addTrack (type, type == TrackType::audio ? TRANS ("Audio") : TRANS ("MIDI"));
            });
        }

        void removeSelectedTrack()
        {
            if (ctx.project == nullptr || uiState.selectedTrack == invalidTrackId) return;

            auto* p = ctx.project.get();
            const auto id = uiState.selectedTrack;
            performProjectEdit (*p, TRANS ("Delete track"), [p, id] { p->removeTrack (id); });
            uiState.select (invalidTrackId, invalidClipId, false);
        }

        void showPreferences()
        {
            juce::Component::SafePointer<MainComponent> safeOwner (&owner);
            auto* self = this;

            PreferencesDialog::launch (ctx, commands, lookAndFeel, [safeOwner, self]
            {
                if (safeOwner != nullptr)
                    self->settingsChanged();
            });
        }

        void settingsChanged()
        {
            if (ctx.settings == nullptr)
                return;

            juce::Desktop::getInstance().setGlobalScaleFactor (ctx.settings->getUiScale());

            if (timeline != nullptr) timeline->setRefreshHz (ctx.settings->getTimelineRefreshHz());
            if (mixer != nullptr)    mixer->setRefreshHz (ctx.settings->getMixerMeterRefreshHz());
            if (ctx.project != nullptr)
                ctx.project->getUndoManager().setMaxNumberOfStoredUnits (
                    ctx.settings->getUndoHistoryLimit() * projectSnapshotActionUnitsPerStep, 1);

            rescanFormatExtensions();

            if (ctx.settings->getLanguage() != currentLanguage)
            {
                currentLanguage = ctx.settings->getLanguage();
                rebuildUi();
                return;
            }

            owner.resized();
            owner.repaint();
        }

        //----------------------------------------------------------------------
        MainComponent& owner;
        AppContext& ctx;

        DarkLookAndFeel lookAndFeel;
        juce::ApplicationCommandManager commands;
        juce::TooltipWindow tooltips { nullptr, 700 };
        UiState uiState;

        View currentView = View::timeline;

        std::unique_ptr<juce::MenuBarComponent> menuBar;
        std::unique_ptr<BrowserPanel>  browser;
        std::unique_ptr<AiPanel>       aiPanel;
        std::unique_ptr<TransportBar>  transport;
        std::unique_ptr<DockContainer> workspace;
        std::vector<std::unique_ptr<FloatingDockWindow>> floatingWindows;
        juce::var pendingLayoutOverride;
        std::unique_ptr<TimelineView>  timeline;
        std::unique_ptr<PianoRollView> pianoRoll;
        std::unique_ptr<MixerView>     mixer;
        std::unique_ptr<TranscribeView> transcribe;
        std::unique_ptr<GenerateView>  generate;
        std::unique_ptr<NotationView>  notation;
        std::unique_ptr<SessionView>     session;
        std::unique_ptr<PlaceholderView> modular;

        std::unique_ptr<juce::StretchableLayoutResizerBar> leftBar, rightBar;
        juce::StretchableLayoutManager layout;

        struct ToolbarEntry { juce::TextButton* button; int width; bool separatorAfter; };
        juce::OwnedArray<juce::TextButton> toolbarButtons;
        std::vector<ToolbarEntry> toolbarLayout;
        juce::ToggleButton snapButton;
        juce::ComboBox gridBox;

        TaskPanel task;
        std::unique_ptr<juce::FileChooser> chooser;

        int  browserWidth = 250, aiWidth = 280;
        bool browserVisible = true, aiVisible = true;
        bool lastPlaying = false, lastRecording = false, lastDirty = false;
        juce::String currentLanguage { "en" };
        juce::Time lastAutoSave { juce::Time::getCurrentTime() };
    };

    //==========================================================================
    MainComponent::MainComponent (AppContext& c)
    {
        impl = std::make_unique<Impl> (*this, c);
        impl->initialise();
        setSize (1500, 900);
    }

    MainComponent::~MainComponent() = default;

    void MainComponent::showView (View v)                { impl->showView (v); }
    MainComponent::View MainComponent::getCurrentView() const noexcept { return impl->currentView; }
    juce::ApplicationCommandManager& MainComponent::getCommandManager() noexcept { return impl->commands; }

    void MainComponent::resized()
    {
        if (impl != nullptr)
            impl->resized();
    }

    void MainComponent::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        // The toolbar strip, drawn under the buttons.
        auto area = getLocalBounds();
        if (impl != nullptr && impl->menuBar != nullptr && impl->menuBar->isVisible())
            area.removeFromTop (menuBarHeight);

        auto strip = area.removeFromTop (toolbarHeight);
        g.setColour (p.headerBg);
        g.fillRect (strip);
        g.setColour (p.divider);
        g.drawHorizontalLine (strip.getBottom() - 1, 0.0f, (float) getWidth());
    }

    //==========================================================================
    void MainComponent::tryToQuit (std::function<void (bool)> callback)
    {
        impl->withUnsavedChangesHandled ([this, callback] (bool proceed)
        {
            impl->saveLayout();
            if (callback) callback (proceed);
        });
    }

    //==========================================================================
    // MenuBarModel
    //==========================================================================
    juce::StringArray MainComponent::getMenuBarNames()
    {
        return { TRANS ("File"), TRANS ("Edit"), TRANS ("Track"), TRANS ("Transport"),
                 TRANS ("View"), TRANS ("AI"), TRANS ("Help") };
    }

    juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
    {
        auto& commands = impl->commands;
        juce::PopupMenu menu;

        switch (index)
        {
            case 0:
                menu.addCommandItem (&commands, CommandIDs::fileNew);
                menu.addCommandItem (&commands, CommandIDs::fileOpen);
                menu.addCommandItem (&commands, CommandIDs::fileSave);
                menu.addCommandItem (&commands, CommandIDs::fileSaveAs);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::importAudio);
                menu.addCommandItem (&commands, CommandIDs::importMidi);
                menu.addCommandItem (&commands, CommandIDs::importDawProject);

                if (impl->ctx.formatExtensions != nullptr)
                {
                    const auto importExtensions = impl->ctx.formatExtensions->matching (ExtensionDirection::importOnly);
                    for (size_t i = 0; i < importExtensions.size(); ++i)
                        menu.addItem ((int) (importExtensionMenuIdBase + i),
                                      importExtensions[i]->name + " (." + importExtensions[i]->fileExtension + ")...");
                }

                menu.addSeparator();
                {
                    juce::PopupMenu exportMenu;
                    exportMenu.addCommandItem (&commands, CommandIDs::exportMidi);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportAudio);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportMusicXml);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportDawProject);

                    if (impl->ctx.formatExtensions != nullptr)
                    {
                        const auto exportExtensions = impl->ctx.formatExtensions->matching (ExtensionDirection::exportOnly);
                        for (size_t i = 0; i < exportExtensions.size(); ++i)
                            exportMenu.addItem ((int) (exportExtensionMenuIdBase + i),
                                                exportExtensions[i]->name + " (." + exportExtensions[i]->fileExtension + ")...");
                    }

                    menu.addSubMenu (TRANS ("Export"), exportMenu);
                }
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::showPreferences);
               #if ! JUCE_MAC
                menu.addSeparator();
                menu.addCommandItem (&commands, juce::StandardApplicationCommandIDs::quit);
               #endif
                break;

            case 1:
                menu.addCommandItem (&commands, juce::StandardApplicationCommandIDs::undo);
                menu.addCommandItem (&commands, juce::StandardApplicationCommandIDs::redo);
                menu.addSeparator();
                menu.addCommandItem (&commands, juce::StandardApplicationCommandIDs::del);
                menu.addCommandItem (&commands, CommandIDs::splitClipAtPlayhead);
                break;

            case 2:
                menu.addCommandItem (&commands, CommandIDs::addAudioTrack);
                menu.addCommandItem (&commands, CommandIDs::addMidiTrack);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::removeSelectedTrack);
                break;

            case 3:
                menu.addCommandItem (&commands, CommandIDs::transportPlay);
                menu.addCommandItem (&commands, CommandIDs::transportStop);
                menu.addCommandItem (&commands, CommandIDs::transportRecord);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::transportReturnToStart);
                menu.addCommandItem (&commands, CommandIDs::transportLoop);
                menu.addCommandItem (&commands, CommandIDs::toggleMetronome);
                break;

            case 4:
                menu.addCommandItem (&commands, CommandIDs::viewTimeline);
                menu.addCommandItem (&commands, CommandIDs::viewPianoRoll);
                menu.addCommandItem (&commands, CommandIDs::viewMixer);
                menu.addCommandItem (&commands, CommandIDs::viewTranscribe);
                menu.addCommandItem (&commands, CommandIDs::viewGenerate);
                menu.addCommandItem (&commands, CommandIDs::viewNotation);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::viewSession);
                menu.addCommandItem (&commands, CommandIDs::viewModular);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::toggleBrowser);
                menu.addCommandItem (&commands, CommandIDs::toggleAiPanel);
                menu.addCommandItem (&commands, CommandIDs::toggleAutomation);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::zoomIn);
                menu.addCommandItem (&commands, CommandIDs::zoomOut);
                menu.addSeparator();
                menu.addItem (20002, TRANS ("Save current layout as startup default"));
                menu.addItem (20003, TRANS ("Reset startup layout (use last session instead)"));
                break;

            case 5:
                menu.addCommandItem (&commands, CommandIDs::transcribeSelection);
                menu.addCommandItem (&commands, CommandIDs::generateFromSelection);
                menu.addSeparator();
                menu.addCommandItem (&commands, CommandIDs::rescanPlugins);
                break;

            case 6:
                menu.addItem (20001, TRANS ("About KANADE DAW"));
                menu.addCommandItem (&commands, CommandIDs::showExtensionHelp);
                break;

            default:
                break;
        }

        return menu;
    }

    void MainComponent::menuItemSelected (int menuItemID, int)
    {
        if (menuItemID >= importExtensionMenuIdBase && menuItemID < exportExtensionMenuIdBase)
        {
            if (impl->ctx.formatExtensions != nullptr)
            {
                const auto extensions = impl->ctx.formatExtensions->matching (ExtensionDirection::importOnly);
                const auto index = (size_t) (menuItemID - importExtensionMenuIdBase);
                if (index < extensions.size())
                    impl->importViaExtension (*extensions[index]);
            }
            return;
        }

        if (menuItemID >= exportExtensionMenuIdBase)
        {
            if (impl->ctx.formatExtensions != nullptr)
            {
                const auto extensions = impl->ctx.formatExtensions->matching (ExtensionDirection::exportOnly);
                const auto index = (size_t) (menuItemID - exportExtensionMenuIdBase);
                if (index < extensions.size())
                    impl->exportViaExtension (*extensions[index]);
            }
            return;
        }

        // Command items invoke themselves through the command manager; only the
        // hand-rolled ids below here need handling.
        if (menuItemID == 20001)
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::InfoIcon,
                TRANS ("About KANADE DAW"),
                juce::String (JUCE_APPLICATION_NAME_STRING) + " "
                  + juce::String (JUCE_APPLICATION_VERSION_STRING) + "\n\n"
                  + TRANS ("AI music generation and transcription in a DAW. "
                           "Distributed free as a single edition."),
                TRANS ("OK"), this);
        else if (menuItemID == 20002)
        {
            if (impl->workspace != nullptr)
                impl->ctx.settings->setDockLayout (DockLayout::startupLayoutName, impl->workspace->toVar());
            impl->ctx.settings->setStartupDockLayoutName (DockLayout::startupLayoutName);

            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::InfoIcon,
                TRANS ("Startup layout saved"),
                TRANS ("KANADE DAW will use this dock layout every time it starts."),
                TRANS ("OK"), this);
        }
        else if (menuItemID == 20003)
        {
            impl->ctx.settings->setStartupDockLayoutName ({});
            impl->ctx.settings->deleteDockLayout (DockLayout::startupLayoutName);
        }
    }

    //==========================================================================
    // ApplicationCommandTarget
    //==========================================================================
    juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget()
    {
        return findFirstTargetParentComponent();
    }

    void MainComponent::getAllCommands (juce::Array<juce::CommandID>& c)
    {
        c.addArray (std::initializer_list<juce::CommandID> {
            CommandIDs::fileNew, CommandIDs::fileOpen, CommandIDs::fileSave, CommandIDs::fileSaveAs,
            CommandIDs::importAudio, CommandIDs::importMidi, CommandIDs::importDawProject,
            CommandIDs::exportMidi, CommandIDs::exportAudio, CommandIDs::exportMusicXml, CommandIDs::exportDawProject,
            juce::StandardApplicationCommandIDs::undo, juce::StandardApplicationCommandIDs::redo,
            juce::StandardApplicationCommandIDs::del, juce::StandardApplicationCommandIDs::quit,
            CommandIDs::addAudioTrack, CommandIDs::addMidiTrack, CommandIDs::removeSelectedTrack,
            CommandIDs::splitClipAtPlayhead,
            CommandIDs::transportPlay, CommandIDs::transportStop, CommandIDs::transportRecord,
            CommandIDs::transportLoop, CommandIDs::transportReturnToStart, CommandIDs::toggleMetronome,
            CommandIDs::viewTimeline, CommandIDs::viewPianoRoll, CommandIDs::viewMixer,
            CommandIDs::viewTranscribe, CommandIDs::viewGenerate, CommandIDs::viewNotation,
            CommandIDs::viewSession, CommandIDs::viewModular,
            CommandIDs::toggleBrowser, CommandIDs::toggleAiPanel, CommandIDs::toggleAutomation,
            CommandIDs::zoomIn, CommandIDs::zoomOut,
            CommandIDs::transcribeSelection, CommandIDs::generateFromSelection,
            CommandIDs::showPreferences, CommandIDs::rescanPlugins, CommandIDs::showExtensionHelp });
    }

    void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
    {
        using KP = juce::KeyPress;
        const auto cmd = juce::ModifierKeys::commandModifier;
        const auto cmdShift = juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier;

        const bool hasProject = impl->ctx.project != nullptr;
        const bool hasClip    = hasProject && impl->uiState.selectedClip != invalidClipId;
        const bool hasTrack   = hasProject && impl->uiState.selectedTrack != invalidTrackId;

        auto viewCommand = [&info, this] (View view, const juce::String& name, const KP& key)
        {
            info.setInfo (name, name, TRANS ("View"), 0);
            info.addDefaultKeypress (key.getKeyCode(), key.getModifiers());
            info.setTicked (impl->currentView == view);
        };

        switch (id)
        {
            case CommandIDs::fileNew:
                info.setInfo (TRANS ("New Project"), TRANS ("Start a new project"), TRANS ("File"), 0);
                info.addDefaultKeypress ('n', cmd);
                break;

            case CommandIDs::fileOpen:
                info.setInfo (TRANS ("Open..."), TRANS ("Open a project"), TRANS ("File"), 0);
                info.addDefaultKeypress ('o', cmd);
                break;

            case CommandIDs::fileSave:
                info.setInfo (TRANS ("Save"), TRANS ("Save the project"), TRANS ("File"), 0);
                info.addDefaultKeypress ('s', cmd);
                info.setActive (hasProject);
                break;

            case CommandIDs::fileSaveAs:
                info.setInfo (TRANS ("Save As..."), TRANS ("Save the project under a new name"), TRANS ("File"), 0);
                info.addDefaultKeypress ('s', cmdShift);
                info.setActive (hasProject);
                break;

            case CommandIDs::importAudio:
                info.setInfo (TRANS ("Import Audio..."), TRANS ("Import audio files"), TRANS ("File"), 0);
                info.addDefaultKeypress ('i', cmd);
                info.setActive (hasProject);
                break;

            case CommandIDs::importMidi:
                info.setInfo (TRANS ("Import MIDI..."), TRANS ("Import a MIDI file"), TRANS ("File"), 0);
                info.addDefaultKeypress ('i', cmdShift);
                info.setActive (hasProject);
                break;

            case CommandIDs::exportMidi:
                info.setInfo (TRANS ("MIDI File..."), TRANS ("Export the project as a MIDI file"), TRANS ("File"), 0);
                info.setActive (hasProject);
                break;

            case CommandIDs::exportAudio:
                info.setInfo (TRANS ("Audio Mixdown..."), TRANS ("Render the project to an audio file"),
                              TRANS ("File"), 0);
                info.setActive (hasProject && ! impl->task.isBusy());
                break;

            case CommandIDs::exportMusicXml:
                info.setInfo (TRANS ("MusicXML..."), TRANS ("Export notation as MusicXML"), TRANS ("File"), 0);
                info.setActive (hasProject);
                break;

            case CommandIDs::importDawProject:
                info.setInfo (TRANS ("Import DAWproject..."), TRANS ("Import a .dawproject file"), TRANS ("File"), 0);
                info.setActive (hasProject);
                break;

            case CommandIDs::exportDawProject:
                info.setInfo (TRANS ("DAWproject..."), TRANS ("Export for Studio One, Bitwig, Cubase and other DAWs"), TRANS ("File"), 0);
                info.setActive (hasProject);
                break;

            case juce::StandardApplicationCommandIDs::undo:
            {
                auto name = TRANS ("Undo");
                if (hasProject && impl->ctx.project->getUndoManager().canUndo())
                    name += " " + impl->ctx.project->getUndoManager().getUndoDescription();
                info.setInfo (name, TRANS ("Undo the last edit"), TRANS ("Edit"), 0);
                info.addDefaultKeypress ('z', cmd);
                info.setActive (hasProject && impl->ctx.project->getUndoManager().canUndo());
                break;
            }

            case juce::StandardApplicationCommandIDs::redo:
            {
                auto name = TRANS ("Redo");
                if (hasProject && impl->ctx.project->getUndoManager().canRedo())
                    name += " " + impl->ctx.project->getUndoManager().getRedoDescription();
                info.setInfo (name, TRANS ("Redo the last undone edit"), TRANS ("Edit"), 0);
                info.addDefaultKeypress ('z', cmdShift);
                info.setActive (hasProject && impl->ctx.project->getUndoManager().canRedo());
                break;
            }

            case juce::StandardApplicationCommandIDs::del:
                info.setInfo (TRANS ("Delete"), TRANS ("Delete the selection"), TRANS ("Edit"), 0);
                info.addDefaultKeypress (KP::deleteKey, juce::ModifierKeys::noModifiers);
                info.setActive (hasClip
                                 || (impl->currentView == View::timeline && impl->timeline != nullptr
                                      && impl->timeline->hasAutomationSelection()));
                break;

            case juce::StandardApplicationCommandIDs::quit:
                info.setInfo (TRANS ("Quit"), TRANS ("Quit KANADE DAW"), TRANS ("File"), 0);
                info.addDefaultKeypress ('q', cmd);
                break;

            case CommandIDs::splitClipAtPlayhead:
                info.setInfo (TRANS ("Split at Playhead"), TRANS ("Split the selected clips at the playhead"),
                              TRANS ("Edit"), 0);
                info.addDefaultKeypress ('e', cmd);
                info.setActive (hasClip);
                break;

            case CommandIDs::addAudioTrack:
                info.setInfo (TRANS ("Add Audio Track"), TRANS ("Add an audio track"), TRANS ("Track"), 0);
                info.addDefaultKeypress ('t', cmd);
                info.setActive (hasProject);
                break;

            case CommandIDs::addMidiTrack:
                info.setInfo (TRANS ("Add MIDI Track"), TRANS ("Add a MIDI track"), TRANS ("Track"), 0);
                info.addDefaultKeypress ('t', cmdShift);
                info.setActive (hasProject);
                break;

            case CommandIDs::removeSelectedTrack:
                info.setInfo (TRANS ("Delete Track"), TRANS ("Delete the selected track"), TRANS ("Track"), 0);
                info.setActive (hasTrack);
                break;

            case CommandIDs::transportPlay:
                info.setInfo (TRANS ("Play/Pause"), TRANS ("Start or pause playback"), TRANS ("Transport"), 0);
                info.addDefaultKeypress (KP::spaceKey, juce::ModifierKeys::noModifiers);
                info.setTicked (impl->lastPlaying);
                break;

            case CommandIDs::transportStop:
                info.setInfo (TRANS ("Stop"), TRANS ("Stop playback"), TRANS ("Transport"), 0);
                info.addDefaultKeypress (KP::numberPad0, juce::ModifierKeys::noModifiers);
                break;

            case CommandIDs::transportRecord:
                info.setInfo (TRANS ("Record"), TRANS ("Start or stop recording"), TRANS ("Transport"), 0);
                info.addDefaultKeypress (KP::F9Key, juce::ModifierKeys::noModifiers);
                info.setTicked (impl->lastRecording);
                break;

            case CommandIDs::transportLoop:
                info.setInfo (TRANS ("Loop"), TRANS ("Loop the marked region"), TRANS ("Transport"), 0);
                info.addDefaultKeypress ('l', cmd);
                info.setTicked (hasProject && impl->ctx.project->loopEnabled);
                break;

            case CommandIDs::transportReturnToStart:
                info.setInfo (TRANS ("Return to Start"), TRANS ("Move the playhead to the start"),
                              TRANS ("Transport"), 0);
                info.addDefaultKeypress (KP::returnKey, juce::ModifierKeys::noModifiers);
                break;

            case CommandIDs::toggleMetronome:
                info.setInfo (TRANS ("Metronome"), TRANS ("Toggle the click"), TRANS ("Transport"), 0);
                info.addDefaultKeypress ('k', cmd);
                info.setTicked (impl->ctx.engine != nullptr
                                  && impl->ctx.engine->getTransport().isMetronomeEnabled());
                break;

            case CommandIDs::viewTimeline:
                viewCommand (View::timeline, TRANS ("Timeline"), KP (KP::F2Key));
                break;
            case CommandIDs::viewPianoRoll:
                viewCommand (View::pianoRoll, TRANS ("Piano Roll"), KP (KP::F3Key));
                break;
            case CommandIDs::viewMixer:
                viewCommand (View::mixer, TRANS ("Mixer"), KP (KP::F4Key));
                break;
            case CommandIDs::viewTranscribe:
                viewCommand (View::transcribe, TRANS ("Transcribe"), KP (KP::F5Key));
                break;
            case CommandIDs::viewGenerate:
                viewCommand (View::generate, TRANS ("Generate"), KP (KP::F6Key));
                break;
            case CommandIDs::viewNotation:
                viewCommand (View::notation, TRANS ("Notation"), KP (KP::F7Key));
                break;
            case CommandIDs::viewSession:
                viewCommand (View::session, TRANS ("Session View"), KP (KP::F8Key));
                break;
            case CommandIDs::viewModular:
                viewCommand (View::modular, TRANS ("Modular Patching"), KP (KP::F11Key));
                break;

            case CommandIDs::toggleBrowser:
                info.setInfo (TRANS ("Browser Panel"), TRANS ("Show or hide the browser"), TRANS ("View"), 0);
                info.addDefaultKeypress ('b', cmd);
                info.setTicked (impl->browserVisible);
                break;

            case CommandIDs::toggleAiPanel:
                info.setInfo (TRANS ("AI Panel"), TRANS ("Show or hide the AI panel"), TRANS ("View"), 0);
                info.addDefaultKeypress ('r', cmd);
                info.setTicked (impl->aiVisible);
                break;

            case CommandIDs::toggleAutomation:
                info.setInfo (TRANS ("Automation"),
                              TRANS ("Show or hide automation lanes for the selected track"),
                              TRANS ("View"), 0);
                info.addDefaultKeypress ('a', cmdShift);
                info.setActive (hasTrack);
                info.setTicked (hasTrack && impl->uiState.isAutomationVisible (impl->uiState.selectedTrack));
                break;

            case CommandIDs::zoomIn:
                info.setInfo (TRANS ("Zoom In"), TRANS ("Zoom the timeline in"), TRANS ("View"), 0);
                info.addDefaultKeypress ('=', cmd);
                break;

            case CommandIDs::zoomOut:
                info.setInfo (TRANS ("Zoom Out"), TRANS ("Zoom the timeline out"), TRANS ("View"), 0);
                info.addDefaultKeypress ('-', cmd);
                break;

            case CommandIDs::transcribeSelection:
                info.setInfo (TRANS ("Transcribe Selection"), TRANS ("Send the selected audio to the transcriber"),
                              TRANS ("AI"), 0);
                info.setActive (hasClip && impl->uiState.selectedClipIsAudio);
                break;

            case CommandIDs::generateFromSelection:
                info.setInfo (TRANS ("Generate from Selection"),
                              TRANS ("Generate arrangements from the selected MIDI"), TRANS ("AI"), 0);
                info.setActive (hasClip && ! impl->uiState.selectedClipIsAudio);
                break;

            case CommandIDs::showPreferences:
                info.setInfo (TRANS ("Preferences..."), TRANS ("Open the preferences"), TRANS ("File"), 0);
                info.addDefaultKeypress (',', cmd);
                break;

            case CommandIDs::rescanPlugins:
                info.setInfo (TRANS ("Rescan Plugins"), TRANS ("Scan for new VST3/AU plugins"), TRANS ("AI"), 0);
                info.setActive (impl->ctx.plugins != nullptr && ! impl->ctx.plugins->isScanning());
                break;

            case CommandIDs::showExtensionHelp:
                info.setInfo (TRANS ("How to build a format extension..."),
                              TRANS ("Shows the manifest schema and command-line contract for format extensions"),
                              TRANS ("Help"), 0);
                break;

            default:
                break;
        }
    }

    bool MainComponent::perform (const InvokedCommandInfo& info)
    {
        auto& ui = impl->uiState;
        auto* project = impl->ctx.project.get();
        auto* engine  = impl->ctx.engine.get();

        switch (info.commandID)
        {
            case CommandIDs::fileNew:     impl->newProject(); return true;
            case CommandIDs::fileOpen:    impl->openProject(); return true;
            case CommandIDs::fileSave:    impl->saveProject ({}); return true;
            case CommandIDs::fileSaveAs:  impl->saveProjectAs ({}); return true;
            case CommandIDs::importAudio: impl->importFiles (true); return true;
            case CommandIDs::importMidi:  impl->importFiles (false); return true;
            case CommandIDs::exportMidi:  impl->exportMidi(); return true;
            case CommandIDs::exportAudio: impl->exportAudio(); return true;
            case CommandIDs::exportMusicXml: impl->exportMusicXml(); return true;
            case CommandIDs::importDawProject: impl->importDawProject(); return true;
            case CommandIDs::exportDawProject: impl->exportDawProject(); return true;

            case juce::StandardApplicationCommandIDs::quit:
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->systemRequestedQuit();
                return true;

            case juce::StandardApplicationCommandIDs::undo:
                if (project != nullptr)
                {
                    project->getUndoManager().undo();
                    project->sendChangeMessage();
                    impl->commands.commandStatusChanged();
                }
                return true;

            case juce::StandardApplicationCommandIDs::redo:
                if (project != nullptr)
                {
                    project->getUndoManager().redo();
                    project->sendChangeMessage();
                    impl->commands.commandStatusChanged();
                }
                return true;

            case juce::StandardApplicationCommandIDs::del:
                // Selected automation breakpoints win while the timeline is up:
                // they are the finer selection, and it says false when there
                // are none.  Another view's Delete must not reach into them.
                if (impl->currentView == View::timeline && impl->timeline != nullptr
                     && impl->timeline->deleteSelectedAutomationPoints())
                    return true;

                if (project != nullptr && ui.selectedClip != invalidClipId)
                {
                    const auto trackId = ui.selectedTrack;
                    const auto clipId  = ui.selectedClip;
                    const bool isAudio = ui.selectedClipIsAudio;

                    performProjectEdit (*project, TRANS ("Delete clip"), [project, trackId, clipId, isAudio]
                    {
                        auto* track = project->findTrack (trackId);
                        if (track == nullptr) return;

                        if (isAudio)
                            track->audioClips.erase (std::remove_if (track->audioClips.begin(),
                                                                     track->audioClips.end(),
                                                                     [clipId] (const AudioClip& c)
                                                                     { return c.id == clipId; }),
                                                     track->audioClips.end());
                        else
                            track->midiClips.erase (std::remove_if (track->midiClips.begin(),
                                                                    track->midiClips.end(),
                                                                    [clipId] (const MidiClip& c)
                                                                    { return c.id == clipId; }),
                                                    track->midiClips.end());
                    });

                    ui.select (trackId, invalidClipId, isAudio);
                }
                return true;

            case CommandIDs::splitClipAtPlayhead:
                if (impl->timeline != nullptr)
                    impl->timeline->splitSelectionAtPlayhead();
                return true;

            case CommandIDs::addAudioTrack: impl->addTrack (TrackType::audio); return true;
            case CommandIDs::addMidiTrack:  impl->addTrack (TrackType::midi); return true;
            case CommandIDs::removeSelectedTrack: impl->removeSelectedTrack(); return true;

            case CommandIDs::transportPlay:
                if (engine != nullptr)
                {
                    if (engine->getTransport().isPlaying()) engine->stop();
                    else                                    engine->play();
                }
                return true;

            case CommandIDs::transportStop:
                if (engine != nullptr)
                {
                    if (engine->getTransport().isRecording()) engine->stopRecording();
                    engine->stop();
                }
                return true;

            case CommandIDs::transportRecord:
                if (engine != nullptr)
                {
                    if (engine->getTransport().isRecording()) engine->stopRecording();
                    else                                      engine->startRecording();
                }
                return true;

            case CommandIDs::transportLoop:
                if (project != nullptr)
                {
                    const bool enabled = ! project->loopEnabled;
                    performProjectEdit (*project, TRANS ("Loop"), [project, enabled]
                    {
                        project->loopEnabled = enabled;
                    });
                }
                return true;

            case CommandIDs::transportReturnToStart:
                if (engine != nullptr)
                    engine->getTransport().returnToStart();
                return true;

            case CommandIDs::toggleMetronome:
                if (engine != nullptr)
                    engine->getTransport().setMetronomeEnabled (! engine->getTransport().isMetronomeEnabled());
                return true;

            case CommandIDs::viewTimeline:   impl->showView (View::timeline); return true;
            case CommandIDs::viewPianoRoll:  impl->showView (View::pianoRoll); return true;
            case CommandIDs::viewMixer:      impl->showView (View::mixer); return true;
            case CommandIDs::viewTranscribe: impl->showView (View::transcribe); return true;
            case CommandIDs::viewGenerate:   impl->showView (View::generate); return true;
            case CommandIDs::viewNotation:   impl->showView (View::notation); return true;
            case CommandIDs::viewSession:    impl->showView (View::session); return true;
            case CommandIDs::viewModular:    impl->showView (View::modular); return true;

            case CommandIDs::toggleBrowser:
                impl->browserVisible = ! impl->browserVisible;
                impl->updatePanelVisibility();
                resized();
                return true;

            case CommandIDs::toggleAiPanel:
                impl->aiVisible = ! impl->aiVisible;
                impl->updatePanelVisibility();
                resized();
                return true;

            case CommandIDs::toggleAutomation:
                if (impl->timeline != nullptr)
                {
                    impl->timeline->toggleAutomationForSelectedTrack();
                    impl->commands.commandStatusChanged();
                }
                return true;

            case CommandIDs::zoomIn:
                if (impl->timeline != nullptr) impl->timeline->zoom (1.25);
                return true;

            case CommandIDs::zoomOut:
                if (impl->timeline != nullptr) impl->timeline->zoom (0.8);
                return true;

            case CommandIDs::transcribeSelection:
                if (project != nullptr && ui.selectedClipIsAudio)
                    if (auto* track = project->findTrack (ui.selectedTrack))
                        if (auto* clip = track->findAudioClip (ui.selectedClip))
                        {
                            const auto& tempo = project->tempo;
                            UiState::TranscribeRequest request;
                            request.valid = true;
                            request.file  = clip->sourceFile;
                            request.offsetSeconds = clip->offsetSeconds;
                            request.lengthSeconds = tempo.beatsToSeconds (clip->endBeats())
                                                      - tempo.beatsToSeconds (clip->startBeats);
                            request.placeAtBeat   = clip->startBeats;
                            request.clipName      = clip->name.isNotEmpty()
                                                      ? clip->name
                                                      : clip->sourceFile.getFileNameWithoutExtension();
                            ui.transcribeRequest = request;
                            ui.sendChangeMessage();
                        }

                impl->showView (View::transcribe);
                return true;

            case CommandIDs::generateFromSelection:
                impl->showView (View::generate);
                return true;

            case CommandIDs::showPreferences: impl->showPreferences(); return true;

            case CommandIDs::rescanPlugins:
                if (impl->ctx.plugins != nullptr)
                    impl->ctx.plugins->startScan (false);
                return true;

            case CommandIDs::showExtensionHelp: ExtensionHelpDialog::launch(); return true;

            default:
                return false;
        }
    }
}
