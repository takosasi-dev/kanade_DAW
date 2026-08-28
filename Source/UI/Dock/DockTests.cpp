#include "Core/Settings.h"
#include "UI/Dock/DockDropZone.h"
#include "UI/Dock/DockLayout.h"
#include "UI/Dock/DockSplit.h"
#include "UI/Dock/DockTabGroup.h"
#include "UI/Dock/FloatingDockWindow.h"
#include <juce_events/juce_events.h>

namespace ss
{

class DockUnitTests final : public juce::UnitTest
{
public:
    DockUnitTests() : juce::UnitTest ("ScoreSmith dock", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("classifyDropZone reads the centre as centre, not an edge");
        {
            const juce::Rectangle<int> bounds (0, 0, 200, 100);
            expect (classifyDropZone (bounds, { 100, 50 }) == DropZone::centre);
        }

        beginTest ("classifyDropZone reads each outer 20% band as its own edge");
        {
            const juce::Rectangle<int> bounds (0, 0, 200, 100);

            expect (classifyDropZone (bounds, { 5, 50 })   == DropZone::left,   "far left");
            expect (classifyDropZone (bounds, { 195, 50 }) == DropZone::right,  "far right");
            expect (classifyDropZone (bounds, { 100, 5 })  == DropZone::top,    "far top");
            expect (classifyDropZone (bounds, { 100, 95 }) == DropZone::bottom, "far bottom");
        }

        beginTest ("classifyDropZone picks the NEAREST edge in a corner, not left-then-top always");
        {
            const juce::Rectangle<int> bounds (0, 0, 200, 100);

            // Top-left corner of a wide-short rect: the point is closer to the
            // top edge (5px away) than the left edge (5px away) is ambiguous by
            // distance alone when they're equal, so use a corner where they
            // clearly differ - closer to the left edge than the top edge here
            // because the rect is wider than it is tall (20% of 200 = 40px from
            // the left, 20% of 100 = 20px from the top - a point 10px from the
            // left and 10px from the top is inside the top band, not the left
            // band, once each edge's threshold is measured in its own axis).
            expect (classifyDropZone (bounds, { 10, 10 }) == DropZone::top,
                    "20% of height (20px) is a closer threshold than 20% of width (40px) here");
        }

        beginTest ("classifyDropZone returns none for a point outside the bounds entirely");
        {
            const juce::Rectangle<int> bounds (0, 0, 200, 100);
            expect (classifyDropZone (bounds, { -10, 50 }) == DropZone::none);
            expect (classifyDropZone (bounds, { 250, 50 }) == DropZone::none);
        }

        beginTest ("classifyDropZone handles a degenerate zero-size rect without dividing by zero");
        {
            const juce::Rectangle<int> empty (0, 0, 0, 0);
            expect (classifyDropZone (empty, { 0, 0 }) == DropZone::none);
        }

        beginTest ("DockTabGroup add/remove/active-index bookkeeping");
        {
            DockTabGroup group;
            expectEquals (group.getNumPanels(), 0);

            juce::Label contentA, contentB, contentC;
            group.addPanel ({ "a", "Panel A", &contentA });
            group.addPanel ({ "b", "Panel B", &contentB });

            expectEquals (group.getNumPanels(), 2);
            expectEquals (group.getPanel (0)->id, juce::String ("a"));
            expectEquals (group.getPanel (1)->id, juce::String ("b"));
            expectEquals (group.indexOfPanel ("b"), 1);
            expectEquals (group.indexOfPanel ("nonexistent"), -1);

            // Insert in the middle.
            group.addPanel ({ "c", "Panel C", &contentC }, 1);
            expectEquals (group.getNumPanels(), 3);
            expectEquals (group.getPanel (1)->id, juce::String ("c"));
            expectEquals (group.getPanel (2)->id, juce::String ("b"));

            group.setActivePanel (2);
            const auto removed = group.removePanel (0);
            expectEquals (removed.id, juce::String ("a"));
            expect (removed.content == &contentA,
                    "removePanel must hand back the same content pointer, not delete it");
            expectEquals (group.getNumPanels(), 2);

            // Removing an out-of-range index is a no-op that returns an empty panel.
            const auto badRemoval = group.removePanel (99);
            expect (badRemoval.id.isEmpty());
            expectEquals (group.getNumPanels(), 2);
        }

        beginTest ("DockTabGroup keeps the correct panel active when a panel is inserted before it");
        {
            DockTabGroup group;

            juce::Label contentA, contentB, contentC;
            group.addPanel ({ "a", "Panel A", &contentA });
            group.addPanel ({ "b", "Panel B", &contentB });
            expectEquals (group.getActiveIndex(), 0);

            // Insert a brand-new panel BEFORE the currently active one (index 0).
            // TabbedButtonBar::addTab re-derives its own current-tab index by
            // tracking the active tab's identity across the shift, not its old
            // numeric position - activeIndex must track the same panel, "a",
            // not stay pinned to the numeric index 0 (which now holds "c").
            group.addPanel ({ "c", "Panel C", &contentC }, 0);

            expectEquals (group.getActiveIndex(), 1, "'a' shifted to index 1 when 'c' was inserted at 0");
            expectEquals (group.getPanel (group.getActiveIndex())->id, juce::String ("a"),
                          "the active panel must still be 'a', not whatever now occupies its old index");
        }

        beginTest ("DockSplit stores direction/ratio and exposes both children");
        {
            auto first = std::make_unique<DockTabGroup>();
            first->addPanel ({ "a", "A", nullptr });
            auto* firstPtr = first.get();

            auto second = std::make_unique<DockTabGroup>();
            second->addPanel ({ "b", "B", nullptr });
            auto* secondPtr = second.get();

            DockSplit split (DockSplit::Direction::vertical, std::move (first), std::move (second), 0.3);

            expect (split.getDirection() == DockSplit::Direction::vertical);
            expectWithinAbsoluteError (split.getRatio(), 0.3, 1.0e-9);
            expect (&split.getFirst() == (DockNode*) firstPtr);
            expect (&split.getSecond() == (DockNode*) secondPtr);
        }

        beginTest ("DockSplit::releaseChildren hands back both children intact, ownership included");
        {
            auto first = std::make_unique<DockTabGroup>();
            auto* firstPtr = first.get();
            auto second = std::make_unique<DockTabGroup>();
            auto* secondPtr = second.get();

            DockSplit split (DockSplit::Direction::horizontal, std::move (first), std::move (second));
            auto [releasedFirst, releasedSecond] = split.releaseChildren();

            expect (releasedFirst.get() == (DockNode*) firstPtr);
            expect (releasedSecond.get() == (DockNode*) secondPtr);
        }

        beginTest ("DockLayout round-trips a split-of-tab-groups tree");
        {
            juce::Label timelineContent, mixerContent;

            auto timelineGroup = std::make_unique<DockTabGroup>();
            timelineGroup->addPanel ({ "timeline", "Timeline", &timelineContent });

            auto mixerGroup = std::make_unique<DockTabGroup>();
            mixerGroup->addPanel ({ "mixer", "Mixer", &mixerContent });

            DockSplit original (DockSplit::Direction::horizontal, std::move (timelineGroup),
                               std::move (mixerGroup), 0.4);
            const auto saved = original.toVar();

            std::map<juce::String, juce::Component*> panelsById {
                { "timeline", &timelineContent }, { "mixer", &mixerContent }
            };
            std::map<juce::String, juce::String> displayNames {
                { "timeline", "Timeline" }, { "mixer", "Mixer" }
            };

            auto restored = DockLayout::restore (saved, panelsById, displayNames);
            expect (restored != nullptr);

            auto* restoredSplit = dynamic_cast<DockSplit*> (restored.get());
            expect (restoredSplit != nullptr);
            expect (restoredSplit->getDirection() == DockSplit::Direction::horizontal);
            expectWithinAbsoluteError (restoredSplit->getRatio(), 0.4, 1.0e-6);

            auto* restoredFirst = dynamic_cast<DockTabGroup*> (&restoredSplit->getFirst());
            expect (restoredFirst != nullptr);
            expectEquals (restoredFirst->getNumPanels(), 1);
            expectEquals (restoredFirst->getPanel (0)->id, juce::String ("timeline"));
            expectEquals (restoredFirst->getPanel (0)->displayName, juce::String ("Timeline"));
            // expectEquals<Component*> won't compile: its failure-message path needs
            // String::operator<< for the value type, and JUCE deletes that overload for
            // bool (which is what a raw pointer decays to here) to block exactly this kind
            // of accidental pointer-to-bool conversion. Existing pointer-identity checks
            // elsewhere in this file (e.g. "removePanel must hand back the same content
            // pointer" above) use plain expect(a == b, ...) for the same reason.
            expect (restoredFirst->getPanel (0)->content == (juce::Component*) &timelineContent,
                    "the restored panel must be wired back to the SAME live content component");

            auto* restoredSecond = dynamic_cast<DockTabGroup*> (&restoredSplit->getSecond());
            expect (restoredSecond != nullptr);
            expectEquals (restoredSecond->getNumPanels(), 1);
            expectEquals (restoredSecond->getPanel (0)->id, juce::String ("mixer"));
            expectEquals (restoredSecond->getPanel (0)->displayName, juce::String ("Mixer"));
            expect (restoredSecond->getPanel (0)->content == (juce::Component*) &mixerContent,
                    "the restored panel must be wired back to the SAME live content component");
        }

        beginTest ("DockLayout::restore drops a panel id no longer present, keeps the rest");
        {
            juce::Label mixerContent;

            auto group = std::make_unique<DockTabGroup>();
            group->addPanel ({ "timeline", "Timeline", nullptr });
            group->addPanel ({ "mixer", "Mixer", nullptr });
            const auto saved = group->toVar();

            // "timeline" is deliberately absent from panelsById.
            std::map<juce::String, juce::Component*> panelsById { { "mixer", &mixerContent } };

            auto restored = DockLayout::restore (saved, panelsById, {});
            auto* restoredGroup = dynamic_cast<DockTabGroup*> (restored.get());
            expect (restoredGroup != nullptr);
            expectEquals (restoredGroup->getNumPanels(), 1, "the missing panel id should be dropped, not crash");
            expectEquals (restoredGroup->getPanel (0)->id, juce::String ("mixer"));
        }

        beginTest ("DockLayout::restore collapses a split to its surviving side when the other side empties out");
        {
            // Same shape as the round-trip test above, but "timeline" is
            // missing from panelsById this time - its tab group restores to
            // nullptr entirely (its only panel was dropped), so the split
            // around it must collapse to just the surviving "mixer" side
            // rather than coming back as a DockSplit with a null child.
            juce::Label mixerContent;

            auto timelineGroup = std::make_unique<DockTabGroup>();
            timelineGroup->addPanel ({ "timeline", "Timeline", nullptr });

            auto mixerGroup = std::make_unique<DockTabGroup>();
            mixerGroup->addPanel ({ "mixer", "Mixer", nullptr });

            DockSplit original (DockSplit::Direction::horizontal, std::move (timelineGroup),
                               std::move (mixerGroup), 0.4);
            const auto saved = original.toVar();

            std::map<juce::String, juce::Component*> panelsById { { "mixer", &mixerContent } };

            auto restored = DockLayout::restore (saved, panelsById, {});
            auto* restoredGroup = dynamic_cast<DockTabGroup*> (restored.get());
            expect (restoredGroup != nullptr,
                    "the split should collapse away entirely, handing back the surviving DockTabGroup directly");
            expectEquals (restoredGroup->getNumPanels(), 1);
            expectEquals (restoredGroup->getPanel (0)->id, juce::String ("mixer"));
        }

        beginTest ("DockLayout::restore returns nullptr for malformed input");
        {
            std::map<juce::String, juce::Component*> panelsById;

            expect (DockLayout::restore (juce::var(), panelsById, {}) == nullptr);
            expect (DockLayout::restore (juce::var (42), panelsById, {}) == nullptr);

            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "not-a-real-type");
            expect (DockLayout::restore (juce::var (obj), panelsById, {}) == nullptr);
        }

        beginTest ("DockLayout::defaultLayout produces a tree containing every supplied id");
        {
            std::map<juce::String, juce::String> names {
                { "timeline", "Timeline" }, { "mixer", "Mixer" }, { "pianoRoll", "Piano Roll" }
            };

            const auto layout = DockLayout::defaultLayout (names);

            // NOTE: the task brief's own version of this test built the
            // placeholder Components with `juce::Label placeholder;` declared
            // INSIDE a `for (auto& kv : panelsById)` loop body and stored
            // `&placeholder` into the map - that Label is destroyed at the
            // end of each loop iteration, so every map entry ends up
            // dangling before restore() is even called. Declaring the
            // Labels here, in the test's own scope, keeps them alive for as
            // long as `restored` (which holds pointers to them as children)
            // is alive.
            juce::Label timelinePlaceholder, mixerPlaceholder, pianoRollPlaceholder;
            std::map<juce::String, juce::Component*> panelsById {
                { "timeline", &timelinePlaceholder },
                { "mixer", &mixerPlaceholder },
                { "pianoRoll", &pianoRollPlaceholder }
            };

            auto restored = DockLayout::restore (layout, panelsById, names);
            expect (restored != nullptr, "the default layout must be well-formed enough to restore itself");

            // Only 3 of the 8 real ids are supplied here, so the "rest"
            // tabbed group defaultLayout() builds for every other id ends
            // up empty and restore() collapses it away entirely (per its
            // documented collapse rule) - the real shape is a 2-level
            // nested split, not the full 3-level 4-column tree.
            auto* outerSplit = dynamic_cast<DockSplit*> (restored.get());
            expect (outerSplit != nullptr, "expected the outer split (timeline | rest)");

            auto* timelineGroup = dynamic_cast<DockTabGroup*> (&outerSplit->getFirst());
            expect (timelineGroup != nullptr);
            expectEquals (timelineGroup->getNumPanels(), 1);
            expectEquals (timelineGroup->getPanel (0)->id, juce::String ("timeline"));
            expectEquals (timelineGroup->getPanel (0)->displayName, juce::String ("Timeline"));

            auto* innerSplit = dynamic_cast<DockSplit*> (&outerSplit->getSecond());
            expect (innerSplit != nullptr,
                    "expected the inner split (mixer | pianoRoll) since the 'rest' tab group is empty and collapses away");

            auto* mixerGroup = dynamic_cast<DockTabGroup*> (&innerSplit->getFirst());
            expect (mixerGroup != nullptr);
            expectEquals (mixerGroup->getNumPanels(), 1);
            expectEquals (mixerGroup->getPanel (0)->id, juce::String ("mixer"));
            expectEquals (mixerGroup->getPanel (0)->displayName, juce::String ("Mixer"));

            auto* pianoRollGroup = dynamic_cast<DockTabGroup*> (&innerSplit->getSecond());
            expect (pianoRollGroup != nullptr);
            expectEquals (pianoRollGroup->getNumPanels(), 1);
            expectEquals (pianoRollGroup->getPanel (0)->id, juce::String ("pianoRoll"));
            expectEquals (pianoRollGroup->getPanel (0)->displayName, juce::String ("Piano Roll"));
        }

        beginTest ("clampToNearestDisplay leaves an already-on-screen rect untouched");
        {
            juce::Displays::Display display;
            display.userArea = { 0, 0, 1920, 1080 };

            const juce::Rectangle<int> onScreen (100, 100, 400, 300);
            const auto result = FloatingDockWindowGeometry::clampToNearestDisplay (onScreen, { display });

            // expectEquals<Rectangle<int>> won't compile: its failure-message path
            // needs String::operator<< for the value type, and JUCE has no such
            // overload for Rectangle<int> - same class of problem as the
            // pointer-identity note above, different type. Plain expect(a == b, ...).
            expect (result == onScreen, "already-on-screen bounds must be returned unchanged");
        }

        beginTest ("clampToNearestDisplay pulls a fully off-screen rect back onto the nearest display");
        {
            juce::Displays::Display display;
            display.userArea = { 0, 0, 1920, 1080 };

            // Saved from a second monitor that's no longer connected - far
            // off to the right and below any real display.
            const juce::Rectangle<int> offScreen (5000, 3000, 400, 300);
            const auto result = FloatingDockWindowGeometry::clampToNearestDisplay (offScreen, { display });

            expect (display.userArea.contains (result),
                    "the corrected bounds must land fully inside the one available display");
            expectEquals (result.getWidth(), 400, "size should be preserved, only position corrected");
            expectEquals (result.getHeight(), 300);
        }

        beginTest ("clampToNearestDisplay with zero displays returns the input unchanged rather than crashing");
        {
            const juce::Rectangle<int> input (10, 10, 200, 200);
            const auto result = FloatingDockWindowGeometry::clampToNearestDisplay (input, {});
            expect (result == input, "zero displays must return the input bounds unchanged");
        }

        beginTest ("clampToNearestDisplay picks the geometrically nearest of several displays, not just the first");
        {
            juce::Displays::Display primary;
            primary.userArea = { 0, 0, 1920, 1080 };

            juce::Displays::Display secondary;
            secondary.userArea = { 1920, 0, 1920, 1080 };

            const juce::Rectangle<int> onSecondary (2500, 400, 400, 300);
            const auto result = FloatingDockWindowGeometry::clampToNearestDisplay (onSecondary, { primary, secondary });

            expect (result == onSecondary,
                    "a rect already fully on the nearest (secondary) display must be returned unchanged");

            const auto resultReordered = FloatingDockWindowGeometry::clampToNearestDisplay (onSecondary, { secondary, primary });
            expect (resultReordered == onSecondary, "display order in the array must not affect which display is chosen");
        }

        beginTest ("Settings persists named dock layouts, round-tripping a real var tree");
        {
            Settings settings;

            expect (settings.getDockLayoutNames().isEmpty()
                    || ! settings.getDockLayoutNames().contains ("Test Layout"),
                    "start from a clean slate for this specific name");

            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "tabGroup");
            juce::Array<juce::var> ids;
            ids.add ("timeline");
            obj->setProperty ("panels", ids);
            obj->setProperty ("active", 0);
            const juce::var layout (obj);

            settings.setDockLayout ("Test Layout", layout);
            expect (settings.getDockLayoutNames().contains ("Test Layout"));

            const auto reloaded = settings.getDockLayout ("Test Layout");
            expect (reloaded.isObject());
            expectEquals (reloaded.getProperty ("type", {}).toString(), juce::String ("tabGroup"));
            expectEquals ((int) reloaded.getProperty ("active", -1), 0);

            const auto* reloadedIds = reloaded.getProperty ("panels", {}).getArray();
            expect (reloadedIds != nullptr);
            expectEquals ((int) reloadedIds->size(), 1);
            expectEquals ((*reloadedIds)[0].toString(), juce::String ("timeline"));

            settings.deleteDockLayout ("Test Layout");
            expect (! settings.getDockLayoutNames().contains ("Test Layout"));
            expect (! settings.getDockLayout ("Test Layout").isObject(),
                    "a deleted (or never-saved) layout name must come back as a non-object var");
        }

        beginTest ("the reserved last-session layout name round-trips the same way as any other");
        {
            Settings settings;

            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "split");
            obj->setProperty ("direction", "horizontal");
            obj->setProperty ("ratio", 0.5);
            const juce::var layout (obj);

            settings.setDockLayout (DockLayout::lastSessionLayoutName, layout);
            const auto reloaded = settings.getDockLayout (DockLayout::lastSessionLayoutName);
            expectEquals (reloaded.getProperty ("direction", {}).toString(), juce::String ("horizontal"));

            settings.deleteDockLayout (DockLayout::lastSessionLayoutName);
        }

        beginTest ("resolveStartupLayoutName falls back to the last-session name when no override is set");
        {
            expectEquals (DockLayout::resolveStartupLayoutName ({}), DockLayout::lastSessionLayoutName);
        }

        beginTest ("resolveStartupLayoutName prefers a non-empty startup override over the last-session name");
        {
            expectEquals (DockLayout::resolveStartupLayoutName (DockLayout::startupLayoutName),
                          DockLayout::startupLayoutName);
            expectEquals (DockLayout::resolveStartupLayoutName ("Some Custom Layout"),
                          juce::String ("Some Custom Layout"));
        }

        beginTest ("a Settings-configured startup layout resolves to the layout the user actually saved");
        {
            Settings settings;

            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "tabGroup");
            juce::Array<juce::var> ids;
            ids.add ("mixer");
            obj->setProperty ("panels", ids);
            obj->setProperty ("active", 0);
            const juce::var startupLayout (obj);

            settings.setDockLayout (DockLayout::startupLayoutName, startupLayout);
            settings.setStartupDockLayoutName (DockLayout::startupLayoutName);

            const auto resolvedName = DockLayout::resolveStartupLayoutName (settings.getStartupDockLayoutName());
            expectEquals (resolvedName, DockLayout::startupLayoutName);

            const auto reloaded = settings.getDockLayout (resolvedName);
            expect (reloaded.isObject());
            const auto* reloadedIds = reloaded.getProperty ("panels", {}).getArray();
            expect (reloadedIds != nullptr && reloadedIds->size() == 1
                    && (*reloadedIds)[0].toString() == "mixer",
                    "resolving by name must land on the layout the user actually saved, not the last-session one");

            settings.setStartupDockLayoutName ({});
            settings.deleteDockLayout (DockLayout::startupLayoutName);

            expectEquals (DockLayout::resolveStartupLayoutName (settings.getStartupDockLayoutName()),
                          DockLayout::lastSessionLayoutName,
                          "clearing the override must fall back to the last-session name again");
        }

        beginTest ("DockContainer fires onPanelDraggedOutside when a drag ends without landing on any target");
        {
            auto group = std::make_unique<DockTabGroup>();
            juce::Label content;
            group->addPanel ({ "x", "Panel X", &content });
            auto* groupPtr = group.get();

            DockContainer container (std::move (group));

            ss::DockPanel captured;
            bool fired = false;
            container.onPanelDraggedOutside = [&] (ss::DockPanel p) { captured = p; fired = true; };

            // Simulate the full lifecycle of a drag that never lands on any
            // DragAndDropTarget: started, then ended with no itemDropped call
            // in between - exactly what a real "dragged past every window's
            // edge" gesture looks like from DockContainer's point of view.
            // A real dock-tab drag's source component is always a
            // DockTabGroup::Tab, i.e. a juce::TabBarButton - DockContainer now
            // checks for exactly that (see isDockTabDrag), so the stand-in has
            // to be one too, parented somewhere under the source group.
            juce::TabbedButtonBar standInBar (juce::TabbedButtonBar::TabsAtTop);
            juce::TabBarButton tabButton ("Panel X", standInBar);
            content.addAndMakeVisible (tabButton);

            const juce::DragAndDropTarget::SourceDetails details (juce::var (0), &tabButton, {});
            container.dragOperationStarted (details);
            container.dragOperationEnded (details);

            expect (fired, "a drag that never landed on any target must fire onPanelDraggedOutside");
            expectEquals (captured.id, juce::String ("x"));
            expectEquals (groupPtr->getNumPanels(), 0, "the dragged panel must be removed from its source group");
        }

        beginTest ("DockContainer does NOT fire onPanelDraggedOutside when itemDropped already handled the drag");
        {
            auto sourceGroup = std::make_unique<DockTabGroup>();
            juce::Label content;
            sourceGroup->addPanel ({ "x", "Panel X", &content });

            DockContainer container (std::move (sourceGroup));

            bool fired = false;
            container.onPanelDraggedOutside = [&] (ss::DockPanel) { fired = true; };

            juce::TabbedButtonBar standInBar (juce::TabbedButtonBar::TabsAtTop);
            juce::TabBarButton tabButton ("Panel X", standInBar);
            content.addAndMakeVisible (tabButton);

            const juce::DragAndDropTarget::SourceDetails details (juce::var (0), &tabButton, {});
            container.dragOperationStarted (details);
            container.itemDropped (details); // simulates a normal, successful in-app drop
            container.dragOperationEnded (details);

            expect (! fired, "onPanelDraggedOutside must not fire when the drag already landed on a real target");
        }

        beginTest ("DockContainer ignores a drag that another feature started inside its subtree");
        {
            // The exact mechanism of the GenerateView bug: CandidateCard calls
            // findParentDragContainerFor(this), which now resolves to the
            // enclosing DockContainer because GenerateView is docked. The drop
            // is claimed by TimelineView - a different DragAndDropTarget - so
            // DockContainer::itemDropped never runs and currentDragWasHandled
            // stays false. Every remaining guard passes: the source component
            // really does sit inside a DockTabGroup, and
            // (int) juce::var ("ss.candidate") is 0 - String::getIntValue on
            // non-numeric text - which is a perfectly valid-looking panel
            // index. Only the source component's TYPE separates the two
            // gestures.
            juce::Label content;

            auto group = std::make_unique<DockTabGroup>();
            group->addPanel ({ "x", "Panel X", &content });
            auto* groupPtr = group.get();

            DockContainer container (std::move (group));

            bool fired = false;
            container.onPanelDraggedOutside = [&] (ss::DockPanel) { fired = true; };

            // A plain Component, not a TabBarButton - standing in for
            // GenerateView's CandidateCard.
            juce::Component foreignDragSource;
            content.addAndMakeVisible (foreignDragSource);

            const juce::DragAndDropTarget::SourceDetails details (juce::var ("ss.candidate"),
                                                                  &foreignDragSource, {});

            expect (! container.isInterestedInDragSource (details),
                    "DockContainer must not claim interest in another feature's drag");

            container.dragOperationStarted (details);
            container.dragOperationEnded (details);

            expect (! fired,
                    "an unrelated feature's drag must never float a dock panel out into a new window");
            expectEquals (groupPtr->getNumPanels(), 1,
                          "the panel at index 0 must still be sitting in its group, untouched");
            const auto* survivor = groupPtr->getPanel (0);
            expect (survivor != nullptr && survivor->id == juce::String ("x"),
                    "panel 0 must still be 'x' - it is exactly what this bug used to rip out");
        }

        beginTest ("DockContainer collapses an emptied source group after a centre-join drop");
        {
            // Plain Components rather than Labels: findTabGroupAt works by
            // getComponentAt hit-testing, so a panel's content component has to
            // actually intercept mouse clicks for the drop point to resolve to
            // the tab group holding it.
            juce::Component contentA, contentB;

            auto groupA = std::make_unique<DockTabGroup>();
            groupA->addPanel ({ "a", "A", &contentA });

            auto groupB = std::make_unique<DockTabGroup>();
            groupB->addPanel ({ "b", "B", &contentB });
            auto* groupBPtr = groupB.get();

            auto rootSplit = std::make_unique<DockSplit> (DockSplit::Direction::horizontal,
                                                          std::move (groupA), std::move (groupB));

            DockContainer container (std::move (rootSplit));
            container.setBounds (0, 0, 400, 200);

            juce::TabbedButtonBar standInBar (juce::TabbedButtonBar::TabsAtTop);
            juce::TabBarButton tabButton ("A", standInBar);
            contentA.addAndMakeVisible (tabButton);

            // Dead centre of groupB - a "join this tab group" drop. The root
            // split fills the container exactly, so a group's own bounds are
            // already in the container-relative coordinates itemDropped wants.
            const auto targetBounds = groupBPtr->getBounds();
            const juce::DragAndDropTarget::SourceDetails details (juce::var (0), &tabButton,
                                                                  targetBounds.getCentre());
            container.itemDropped (details);

            expectEquals (groupBPtr->getNumPanels(), 2, "both panels should now live in the target group");

            if (groupBPtr->getNumPanels() == 2)
            {
                expectEquals (groupBPtr->getPanel (0)->id, juce::String ("b"));
                expectEquals (groupBPtr->getPanel (1)->id, juce::String ("a"));
            }

            // groupA is empty and therefore gone, split and all - the surviving
            // group is now the container's whole tree. (groupA itself has been
            // destroyed by this point, so nothing below looks at it.)
            expect (&container.getRoot() == static_cast<DockNode*> (groupBPtr),
                    "the split must collapse to its surviving child, not leave an empty group behind");
        }

        beginTest ("DockContainer collapses an emptied source group after an edge-split drop");
        {
            juce::Component contentA, contentB;

            auto groupA = std::make_unique<DockTabGroup>();
            groupA->addPanel ({ "a", "A", &contentA });

            auto groupB = std::make_unique<DockTabGroup>();
            groupB->addPanel ({ "b", "B", &contentB });
            auto* groupBPtr = groupB.get();

            auto rootSplit = std::make_unique<DockSplit> (DockSplit::Direction::horizontal,
                                                          std::move (groupA), std::move (groupB));

            DockContainer container (std::move (rootSplit));
            container.setBounds (0, 0, 400, 200);

            juce::TabbedButtonBar standInBar (juce::TabbedButtonBar::TabsAtTop);
            juce::TabBarButton tabButton ("A", standInBar);
            contentA.addAndMakeVisible (tabButton);

            // A few pixels inside groupB's right edge, vertically centred:
            // comfortably inside classifyDropZone's outer 20% right band and
            // well clear of the top/bottom ones.
            const auto targetBounds = groupBPtr->getBounds();
            const juce::Point<int> dropPoint (targetBounds.getRight() - 4, targetBounds.getCentreY());

            const juce::DragAndDropTarget::SourceDetails details (juce::var (0), &tabButton, dropPoint);
            container.itemDropped (details);

            auto* resultSplit = dynamic_cast<DockSplit*> (&container.getRoot());
            expect (resultSplit != nullptr, "an edge drop must leave a split at the root");

            if (resultSplit != nullptr)
            {
                // Without the collapse the root's first child would still be the
                // emptied groupA, with the new split buried one level deeper -
                // this identity check is what proves the collapse actually ran.
                expect (&resultSplit->getFirst() == static_cast<DockNode*> (groupBPtr),
                        "the emptied source group must be folded out, leaving the target as the split's first child");
                expectEquals (groupBPtr->getNumPanels(), 1);

                if (groupBPtr->getNumPanels() == 1)
                    expectEquals (groupBPtr->getPanel (0)->id, juce::String ("b"));

                auto* movedGroup = dynamic_cast<DockTabGroup*> (&resultSplit->getSecond());
                expect (movedGroup != nullptr, "the dragged panel's new group should be on the right");

                if (movedGroup != nullptr && movedGroup->getNumPanels() == 1)
                {
                    expectEquals (movedGroup->getPanel (0)->id, juce::String ("a"));
                    expect (movedGroup->getPanel (0)->content == (juce::Component*) &contentA);
                }
                else
                {
                    expect (false, "expected exactly one panel in the dragged panel's new group");
                }
            }
        }

        beginTest ("DockContainer::extractAllPanels empties a mixed split/tab-group tree and hands back every panel");
        {
            juce::Label contentA, contentB, contentC;

            auto left = std::make_unique<DockTabGroup>();
            left->addPanel ({ "a", "A", &contentA });
            auto* leftPtr = left.get();

            auto rightTop = std::make_unique<DockTabGroup>();
            rightTop->addPanel ({ "b", "B", &contentB });
            rightTop->addPanel ({ "c", "C", &contentC });
            auto* rightTopPtr = rightTop.get();

            // Deliberately already empty - the walk must cope with it.
            auto rightBottom = std::make_unique<DockTabGroup>();
            auto* rightBottomPtr = rightBottom.get();

            auto right = std::make_unique<DockSplit> (DockSplit::Direction::vertical,
                                                      std::move (rightTop), std::move (rightBottom));
            auto rootSplit = std::make_unique<DockSplit> (DockSplit::Direction::horizontal,
                                                          std::move (left), std::move (right));

            DockContainer container (std::move (rootSplit));

            const auto extracted = container.extractAllPanels();

            expectEquals ((int) extracted.size(), 3);

            if (extracted.size() == 3)
            {
                expectEquals (extracted[0].id, juce::String ("a"), "depth-first, left-to-right, tab order");
                expectEquals (extracted[1].id, juce::String ("b"));
                expectEquals (extracted[2].id, juce::String ("c"));
                expect (extracted[0].content == (juce::Component*) &contentA,
                        "content pointers must come back intact - extractAllPanels never owns or deletes them");
                expect (extracted[2].content == (juce::Component*) &contentC);
            }

            expectEquals (leftPtr->getNumPanels(), 0, "every group in the tree must be left empty");
            expectEquals (rightTopPtr->getNumPanels(), 0);
            expectEquals (rightBottomPtr->getNumPanels(), 0);
            expectEquals ((int) container.extractAllPanels().size(), 0,
                          "a second call on the now-empty tree returns nothing");
        }
    }
};

static DockUnitTests dockUnitTests;

}
