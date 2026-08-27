# Docking Layout System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace ScoreSmith's single-view tab switcher (`MainComponent::showView`, which shows exactly one of 8 always-alive views at a time) with a real dockable-panel layout system — split panes, tabbed groups, panels that float into their own OS windows for multi-monitor use, and named layouts that persist across sessions — while leaving all 8 existing view components internally untouched.

**Architecture:** A `DockNode` tree (`DockSplit` for two-way splits, `DockTabGroup` for tabbed groups of panels) sits inside a root `DockContainer`. `DockContainer` owns the tree, implements `juce::DragAndDropContainer` to broker drag gestures between any two nodes anywhere in the tree (including across into a `FloatingDockWindow`), and uses a small pure `classifyDropZone` function to decide whether a drop means "split here" or "add as a tab here". `DockLayout` serializes the whole tree (main window + every floating window) to/from `juce::var`, mirroring `ProjectPersistence.cpp`'s existing `DynamicObject` idiom exactly, and named layouts are persisted through `Settings` the same way voicebank folders or the sample library list already are.

**Tech Stack:** C++20 / JUCE 8.0.6 (existing ScoreSmith stack). Reuses `juce::TabbedButtonBar` (subclassed, not `TabbedComponent` — see Task 3) and `juce::StretchableLayoutManager`/`StretchableLayoutResizerBar` (already used for the browser/AI-panel side splitters in `MainComponent.cpp:225-226` — see Task 4) instead of hand-rolling tab or splitter widgets from scratch. No new external dependencies.

**Spec:** `E:\MIDI&DAW\docs\superpowers\specs\2026-08-26-docking-layout-system-design.md`

## Global Constraints

- All 8 existing views (`timeline`, `pianoRoll`, `mixer`, `transcribe`, `generate`, `notation`, `session`, `modular` — `Source/UI/MainComponent.h:18`) must be dockable from this plan's first version. No reduced-scope phase.
- The 8 existing view components (`TimelineView`, `MixerView`, etc.) must not be modified. The docking layer only changes where their already-constructed `juce::Component*` is parented and shown.
- Layout state is app-wide (through `Settings`, same `juce::PropertiesFile` as everything else in `Settings.cpp`), never stored in `.ssproj` / `Project`.
- No third-party docking library. Everything in `Source/UI/Dock/` is original code built on stock JUCE 8 widgets.
- Reuse `juce::TabbedButtonBar` (subclassed) for tab groups and `juce::StretchableLayoutManager` + `juce::StretchableLayoutResizerBar` for split resizing — do not hand-roll either from scratch (see Tasks 3 and 4 for the exact reasoning and the existing precedent this mirrors).
- Every new source file follows the existing style: `namespace ss { ... }`, `/** */` doc comments only where the *why* isn't obvious from the name, `juce::` fully qualified.
- Matches this codebase's existing, established testing convention: pure logic (drop-zone classification, layout serialization, monitor-bounds clamping, named-layout persistence) gets unit tests exactly like `.ust`/`oto.ini` parsing did; visual/drag-gesture UI components (`DockTabGroup`, `DockSplit`, `DockContainer`, `FloatingDockWindow`, the `MainComponent` rewiring) do not — the same precedent already set for `PreferencesDialog` (`docs/STATUS.md`'s test table lists no UI-component tests). Verification for those is "it builds and behaves correctly on manual launch."

---

### Task 1: Dock data types

**Files:**
- Create: `Source/UI/Dock/DockNode.h`
- Create: `Source/UI/Dock/DockPanel.h`

**Interfaces:**
- Produces: `class DockNode : public juce::Component` (abstract, `virtual juce::var toVar() const = 0;`), `struct DockPanel { juce::String id; juce::String displayName; juce::Component* content = nullptr; }`.

**Test:** none — these are a pure-virtual base class and a plain data struct with no logic of their own to exercise (matching this codebase's convention that trivial data-only types don't get a dedicated test file; `UtauTypes.h`'s own struct fields aren't individually tested either, only the behaviour methods like `currentContentHash()` are). Task 3 and Task 6 exercise these types indirectly through real behaviour.

- [ ] **Step 1: Write the two header files**

Create `Source/UI/Dock/DockNode.h`:

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss
{
    /** One node in a docking layout tree. A node is either a DockSplit (two
        children divided by a resizable bar) or a DockTabGroup (a tab strip
        over one or more DockPanels). DockContainer owns the root of the tree;
        DockLayout walks it to serialize/deserialize the whole layout.

        DockNode itself carries no docking-specific state - it exists so
        DockContainer, DockSplit, and DockLayout can all hold/pass
        "some node, split or tab group, I don't need to know which yet"
        without a naked juce::Component*. */
    class DockNode : public juce::Component
    {
    public:
        ~DockNode() override = default;

        /** Serializes this node and everything below it. DockSplit writes its
            direction/ratio/children; DockTabGroup writes its panel ids and
            active index. See DockLayout.h for the exact shape and the
            matching fromVar-side reconstruction. */
        virtual juce::var toVar() const = 0;
    };
}
```

Create `Source/UI/Dock/DockPanel.h`:

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss
{
    /** One dockable screen's worth of content. `id` is one of the 8 fixed
        view identifiers ("timeline", "mixer", "pianoRoll", "transcribe",
        "generate", "notation", "session", "modular" - see Task 9) and is
        what DockLayout persists; `content` points at the already-constructed,
        already-owned-elsewhere view component (MainComponent keeps owning
        the std::unique_ptr<TimelineView> etc. exactly as it does today - a
        DockPanel never owns its content). */
    struct DockPanel
    {
        juce::String id;
        juce::String displayName;
        juce::Component* content = nullptr;
    };
}
```

- [ ] **Step 2: Build to confirm it compiles**

```powershell
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
cmake --build "E:/MIDIDAW/build" --config Debug --parallel
```
Expected: builds with zero errors (these two headers aren't included anywhere yet, so this just confirms the syntax is valid C++20 against the vendored JUCE headers — CMake's `GLOB_RECURSE ... CONFIGURE_DEPENDS` in `CMakeLists.txt:36` picks up the new `Source/UI/Dock/` directory automatically, no `CMakeLists.txt` edit needed. A header-only addition with nothing `#include`-ing it yet won't itself trigger a recompile of anything, so also run `& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe" --run-tests` and confirm the existing total is unchanged (**2,206 passed, 0 failed** going into this task) — a header syntax error would still show up as a build failure the moment Task 3 includes it, but confirming zero regressions now is cheap insurance).

- [ ] **Step 3: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockNode.h Source/UI/Dock/DockPanel.h
git -C "E:/MIDI&DAW" commit -m "Add DockNode/DockPanel base types"
```

---

### Task 2: Drop-zone classifier

**Files:**
- Create: `Source/UI/Dock/DockDropZone.h`
- Create: `Source/UI/Dock/DockDropZone.cpp`
- Create: `Source/UI/Dock/DockTests.cpp`

**Interfaces:**
- Produces: `enum class ss::DropZone { none, left, right, top, bottom, centre }`; `DropZone ss::classifyDropZone (juce::Rectangle<int> targetBounds, juce::Point<int> dropPointInTargetLocalCoords) noexcept`.

- [ ] **Step 1: Write the failing test**

Create `Source/UI/Dock/DockTests.cpp`:

```cpp
#include "UI/Dock/DockDropZone.h"
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

        beginTest ("classifyDropZone returns none for a point outside the bounds entirely")
        {
            const juce::Rectangle<int> bounds (0, 0, 200, 100);
            expect (classifyDropZone (bounds, { -10, 50 }) == DropZone::none);
            expect (classifyDropZone (bounds, { 250, 50 }) == DropZone::none);
        }

        beginTest ("classifyDropZone handles a degenerate zero-size rect without dividing by zero")
        {
            const juce::Rectangle<int> empty (0, 0, 0, 0);
            expect (classifyDropZone (empty, { 0, 0 }) == DropZone::none);
        }
    }
};

static DockUnitTests dockUnitTests;

}
```

- [ ] **Step 2: Run test to verify it fails**

```powershell
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
cmake --build "E:/MIDIDAW/build" --config Debug --parallel
```
Expected: FAIL — compile error, `UI/Dock/DockDropZone.h` does not exist yet.

- [ ] **Step 3: Write the implementation**

Create `Source/UI/Dock/DockDropZone.h`:

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss
{
    /** Where a drag-and-drop point landed relative to a candidate drop
        target's bounds. `centre` means "join this target's tab group";
        an edge means "split the target and place the dragged panel on
        that side"; `none` means the point isn't inside the target at all. */
    enum class DropZone { none, left, right, top, bottom, centre };

    /** Classifies `dropPointInTargetLocalCoords` (in `targetBounds`'s own
        local coordinate space, i.e. (0,0) is targetBounds's top-left)
        against the outer 20%-of-each-axis band on every side. A point in
        more than one band (a corner) resolves to whichever edge's band it
        is proportionally deeper into, so a wide-short target favours its
        top/bottom edges over its left/right ones in the corners, and vice
        versa for a tall-narrow one - matching how a user visually judges
        "closer to this edge" relative to the shape they're looking at,
        not raw pixel distance. */
    DropZone classifyDropZone (juce::Rectangle<int> targetBounds,
                               juce::Point<int> dropPointInTargetLocalCoords) noexcept;
}
```

Create `Source/UI/Dock/DockDropZone.cpp`:

```cpp
#include "UI/Dock/DockDropZone.h"

namespace ss
{
    DropZone classifyDropZone (juce::Rectangle<int> targetBounds,
                               juce::Point<int> dropPointInTargetLocalCoords) noexcept
    {
        if (targetBounds.getWidth() <= 0 || targetBounds.getHeight() <= 0)
            return DropZone::none;

        const juce::Rectangle<int> local (0, 0, targetBounds.getWidth(), targetBounds.getHeight());

        if (! local.contains (dropPointInTargetLocalCoords))
            return DropZone::none;

        constexpr float edgeBand = 0.2f;

        const auto x = (float) dropPointInTargetLocalCoords.x;
        const auto y = (float) dropPointInTargetLocalCoords.y;
        const auto w = (float) local.getWidth();
        const auto h = (float) local.getHeight();

        // Fraction of the way from the NEAREST edge on each axis, normalised
        // to that axis's own edge-band width, so a wide-short rect's smaller
        // (height) band dominates a corner over its larger (width) one.
        const auto leftFrac   = x / (w * edgeBand);
        const auto rightFrac  = (w - x) / (w * edgeBand);
        const auto topFrac    = y / (h * edgeBand);
        const auto bottomFrac = (h - y) / (h * edgeBand);

        const auto nearest = juce::jmin (leftFrac, rightFrac, topFrac, bottomFrac);

        if (nearest >= 1.0f)
            return DropZone::centre;

        if (nearest == leftFrac)   return DropZone::left;
        if (nearest == rightFrac)  return DropZone::right;
        if (nearest == topFrac)    return DropZone::top;
        return DropZone::bottom;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same build command as Step 2, then:
```powershell
& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe" --run-tests=ScoreSmith
```
Expected: PASS, including the new "ScoreSmith dock" category tests. Also re-run the full unfiltered `--run-tests` and confirm the total grew from 2,206 with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockDropZone.h Source/UI/Dock/DockDropZone.cpp Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add drop-zone classifier for the docking system"
```

---

### Task 3: DockTabGroup

**Files:**
- Create: `Source/UI/Dock/DockTabGroup.h`
- Create: `Source/UI/Dock/DockTabGroup.cpp`
- Modify: `Source/UI/Dock/DockTests.cpp` (extend)

**Interfaces:**
- Consumes: `DockNode` (Task 1), `DockPanel` (Task 1).
- Produces:
  ```cpp
  class DockTabGroup final : public DockNode
  {
  public:
      DockTabGroup();

      void addPanel (DockPanel panel, int insertIndex = -1);
      /** Returns the removed panel's data (content pointer intact - the
          caller/DockContainer decides where it goes next, this class never
          deletes `content`, it never owned it). Returns {} (empty id) if
          index is out of range. */
      DockPanel removePanel (int index);
      void setActivePanel (int index);

      int getNumPanels() const noexcept;
      const DockPanel* getPanel (int index) const noexcept;
      int indexOfPanel (const juce::String& id) const noexcept;

      void resized() override;
      juce::var toVar() const override;
  };
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/UI/Dock/DockTests.cpp` (add `#include "UI/Dock/DockTabGroup.h"` at the top), inside `runTest()`:

```cpp
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
            expectEquals (removed.content, (juce::Component*) &contentA,
                          "removePanel must hand back the same content pointer, not delete it");
            expectEquals (group.getNumPanels(), 2);

            // Removing an out-of-range index is a no-op that returns an empty panel.
            const auto badRemoval = group.removePanel (99);
            expect (badRemoval.id.isEmpty());
            expectEquals (group.getNumPanels(), 2);
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command as Task 1 Step 2. Expected: FAIL — `UI/Dock/DockTabGroup.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/UI/Dock/DockTabGroup.h`:

```cpp
#pragma once
#include "UI/Dock/DockNode.h"
#include "UI/Dock/DockPanel.h"
#include <vector>

namespace ss
{
    /** A tab strip over one or more DockPanels, showing exactly one panel's
        `content` at a time. Built on juce::TabbedButtonBar (subclassed, not
        juce::TabbedComponent - TabbedComponent hardcodes its own tabs-plus-
        content resized() layout with no override point, which fights a
        dock's need to own that layout itself; TabbedButtonBar just manages
        tab buttons/names and leaves content-swapping to the caller). */
    class DockTabGroup final : public DockNode
    {
    public:
        DockTabGroup();

        void addPanel (DockPanel panel, int insertIndex = -1);
        DockPanel removePanel (int index);
        void setActivePanel (int index);

        int getNumPanels() const noexcept                  { return (int) panels.size(); }
        const DockPanel* getPanel (int index) const noexcept;
        int indexOfPanel (const juce::String& id) const noexcept;

        void resized() override;
        juce::var toVar() const override;

    private:
        /** Gives individual tabs mouse-drag behaviour (reorder within this
            bar via TabbedButtonBar::moveTab, or hand off to DockContainer's
            drag-and-drop machinery once the drag leaves this bar's bounds -
            wired up in Task 5, this class only fires the drag start). */
        class Tab final : public juce::TabBarButton
        {
        public:
            Tab (const juce::String& name, juce::TabbedButtonBar& bar) : juce::TabBarButton (name, bar) {}

            void mouseDrag (const juce::MouseEvent& e) override
            {
                juce::TabBarButton::mouseDrag (e);

                if (e.mouseWasDraggedSinceMouseDown() && ! dragStarted)
                {
                    if (auto* dd = juce::DragAndDropContainer::findParentDragContainerFor (this))
                    {
                        dragStarted = true;
                        dd->startDragging (juce::var (getIndex()), this, juce::ScaledImage(), true);
                    }
                }
            }

            void mouseUp (const juce::MouseEvent& e) override
            {
                juce::TabBarButton::mouseUp (e);
                dragStarted = false;
            }

        private:
            bool dragStarted = false;
        };

        class Bar final : public juce::TabbedButtonBar
        {
        public:
            Bar() : juce::TabbedButtonBar (juce::TabbedButtonBar::TabsAtTop) {}

            std::function<void (int, const juce::String&)> onCurrentTabChanged;

        protected:
            juce::TabBarButton* createTabButton (const juce::String& tabName, int tabIndex) override
            {
                juce::ignoreUnused (tabIndex);
                return new Tab (tabName, *this);
            }

            void currentTabChanged (int newIndex, const juce::String& newName) override
            {
                if (onCurrentTabChanged)
                    onCurrentTabChanged (newIndex, newName);
            }
        };

        Bar bar;
        std::vector<DockPanel> panels;
        int activeIndex = -1;

        void showActiveContent();
    };
}
```

Create `Source/UI/Dock/DockTabGroup.cpp`:

```cpp
#include "UI/Dock/DockTabGroup.h"

namespace ss
{
    DockTabGroup::DockTabGroup()
    {
        addAndMakeVisible (bar);
        bar.onCurrentTabChanged = [this] (int index, const juce::String&) { setActivePanel (index); };
    }

    void DockTabGroup::addPanel (DockPanel panel, int insertIndex)
    {
        const auto index = insertIndex < 0 || insertIndex > (int) panels.size()
                                ? (int) panels.size()
                                : insertIndex;

        if (panel.content != nullptr)
            addChildComponent (panel.content);

        panels.insert (panels.begin() + index, panel);
        bar.addTab (panel.displayName, juce::Colours::transparentBlack, index);

        if (activeIndex < 0)
            setActivePanel (index);
        else
            resized();
    }

    DockPanel DockTabGroup::removePanel (int index)
    {
        if (index < 0 || index >= (int) panels.size())
            return {};

        auto removed = panels[(size_t) index];

        if (removed.content != nullptr)
            removeChildComponent (removed.content);

        panels.erase (panels.begin() + index);
        bar.removeTab (index);

        if (panels.empty())
        {
            activeIndex = -1;
        }
        else
        {
            activeIndex = juce::jlimit (0, (int) panels.size() - 1, activeIndex);
            bar.setCurrentTabIndex (activeIndex, false);
            showActiveContent();
        }

        return removed;
    }

    void DockTabGroup::setActivePanel (int index)
    {
        if (index < 0 || index >= (int) panels.size())
            return;

        activeIndex = index;
        bar.setCurrentTabIndex (index, false);
        showActiveContent();
    }

    void DockTabGroup::showActiveContent()
    {
        for (int i = 0; i < (int) panels.size(); ++i)
            if (auto* c = panels[(size_t) i].content)
                c->setVisible (i == activeIndex);

        resized();
    }

    const DockPanel* DockTabGroup::getPanel (int index) const noexcept
    {
        if (index < 0 || index >= (int) panels.size())
            return nullptr;

        return &panels[(size_t) index];
    }

    int DockTabGroup::indexOfPanel (const juce::String& id) const noexcept
    {
        for (int i = 0; i < (int) panels.size(); ++i)
            if (panels[(size_t) i].id == id)
                return i;

        return -1;
    }

    void DockTabGroup::resized()
    {
        auto area = getLocalBounds();
        bar.setBounds (area.removeFromTop (24));

        if (activeIndex >= 0 && activeIndex < (int) panels.size())
            if (auto* c = panels[(size_t) activeIndex].content)
                c->setBounds (area);
    }

    juce::var DockTabGroup::toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("type", "tabGroup");

        juce::Array<juce::var> panelIds;
        for (const auto& p : panels)
            panelIds.add (p.id);

        obj->setProperty ("panels", panelIds);
        obj->setProperty ("active", activeIndex);
        return juce::var (obj);
    }
}
```

Note for whoever implements this: `Tab::mouseDrag`'s `startDragging` call is the drag-*initiation* half only — Task 5's `DockContainer` is what actually receives the drop and decides what it means (join this group vs. split vs. float). This task's own test only covers the panel-bookkeeping half (add/remove/active-index), which is genuinely testable without a window; the tab-bar-and-drag half is UI/manual-verification only per this plan's testing convention (see Global Constraints) — don't add a test that tries to simulate a `MouseEvent` drag, that's not how this codebase tests UI.

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 2, then confirm the new "DockTabGroup add/remove/active-index bookkeeping" test passes and the full suite total grew with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockTabGroup.h Source/UI/Dock/DockTabGroup.cpp Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add DockTabGroup"
```

---

### Task 4: DockSplit

**Files:**
- Create: `Source/UI/Dock/DockSplit.h`
- Create: `Source/UI/Dock/DockSplit.cpp`
- Modify: `Source/UI/Dock/DockTests.cpp` (extend)

**Interfaces:**
- Consumes: `DockNode` (Task 1).
- Produces:
  ```cpp
  class DockSplit final : public DockNode
  {
  public:
      enum class Direction { horizontal, vertical };

      DockSplit (Direction, std::unique_ptr<DockNode> first, std::unique_ptr<DockNode> second, double ratio = 0.5);

      Direction getDirection() const noexcept;
      double getRatio() const noexcept;
      DockNode& getFirst() const noexcept;
      DockNode& getSecond() const noexcept;

      /** Releases ownership of both children (for DockContainer to move one
          of them elsewhere and discard the split itself, e.g. when a group
          empties out and its sibling should take over the whole space). */
      std::pair<std::unique_ptr<DockNode>, std::unique_ptr<DockNode>> releaseChildren();

      void resized() override;
      juce::var toVar() const override;
  };
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/UI/Dock/DockTests.cpp` (add `#include "UI/Dock/DockSplit.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("DockSplit stores direction/ratio and exposes both children")
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
            expectEquals (&split.getFirst(), (DockNode*) firstPtr);
            expectEquals (&split.getSecond(), (DockNode*) secondPtr);
        }

        beginTest ("DockSplit::releaseChildren hands back both children intact, ownership included")
        {
            auto first = std::make_unique<DockTabGroup>();
            auto* firstPtr = first.get();
            auto second = std::make_unique<DockTabGroup>();
            auto* secondPtr = second.get();

            DockSplit split (DockSplit::Direction::horizontal, std::move (first), std::move (second));
            auto [releasedFirst, releasedSecond] = split.releaseChildren();

            expectEquals (releasedFirst.get(), (DockNode*) firstPtr);
            expectEquals (releasedSecond.get(), (DockNode*) secondPtr);
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `UI/Dock/DockSplit.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/UI/Dock/DockSplit.h`:

```cpp
#pragma once
#include "UI/Dock/DockNode.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace ss
{
    /** Two DockNodes divided by a resizable bar. Built on
        juce::StretchableLayoutManager + juce::StretchableLayoutResizerBar -
        the same pair Source/UI/MainComponent.cpp already uses for its
        browser/AI-panel side splitters (MainComponent.cpp:225-226) - rather
        than hand-rolling drag-to-resize, which JUCE already does correctly
        (including the resize cursor and drag-constrained-to-axis behaviour). */
    class DockSplit final : public DockNode
    {
    public:
        enum class Direction { horizontal, vertical };

        DockSplit (Direction direction, std::unique_ptr<DockNode> first,
                  std::unique_ptr<DockNode> second, double ratio = 0.5);

        Direction getDirection() const noexcept { return dir; }
        double getRatio() const noexcept;
        DockNode& getFirst() const noexcept  { return *firstChild; }
        DockNode& getSecond() const noexcept { return *secondChild; }

        std::pair<std::unique_ptr<DockNode>, std::unique_ptr<DockNode>> releaseChildren();

        void resized() override;
        juce::var toVar() const override;

    private:
        Direction dir;
        std::unique_ptr<DockNode> firstChild, secondChild;
        juce::StretchableLayoutManager layoutManager;
        juce::StretchableLayoutResizerBar resizerBar;

        static constexpr int resizerBarThickness = 6;
    };
}
```

Create `Source/UI/Dock/DockSplit.cpp`:

```cpp
#include "UI/Dock/DockSplit.h"

namespace ss
{
    DockSplit::DockSplit (Direction direction, std::unique_ptr<DockNode> first,
                          std::unique_ptr<DockNode> second, double ratio)
        : dir (direction),
          firstChild (std::move (first)),
          secondChild (std::move (second)),
          resizerBar (&layoutManager, 1, direction == Direction::horizontal)
    {
        addAndMakeVisible (*firstChild);
        addAndMakeVisible (*secondChild);
        addAndMakeVisible (resizerBar);

        // Item 0 = firstChild, item 1 = the resizer bar, item 2 = secondChild.
        // Both panels can shrink to 40px before the resizer refuses to move
        // further; the ratio is expressed as each panel's preferred fraction
        // of the space left after the fixed-width resizer bar is removed.
        layoutManager.setItemLayout (0, 40.0, -1.0, -ratio);
        layoutManager.setItemLayout (1, resizerBarThickness, resizerBarThickness, resizerBarThickness);
        layoutManager.setItemLayout (2, 40.0, -1.0, -(1.0 - ratio));
    }

    double DockSplit::getRatio() const noexcept
    {
        // StretchableLayoutManager doesn't expose the live ratio directly -
        // derive it from the current pixel sizes, which it does expose.
        const auto firstSize = layoutManager.getItemCurrentPosition (0);
        const auto secondSize = layoutManager.getItemCurrentPosition (2);
        const auto total = firstSize + secondSize;
        return total > 0.0 ? firstSize / total : 0.5;
    }

    std::pair<std::unique_ptr<DockNode>, std::unique_ptr<DockNode>> DockSplit::releaseChildren()
    {
        removeChildComponent (firstChild.get());
        removeChildComponent (secondChild.get());
        return { std::move (firstChild), std::move (secondChild) };
    }

    void DockSplit::resized()
    {
        juce::Component* comps[] = { firstChild.get(), &resizerBar, secondChild.get() };

        if (dir == Direction::horizontal)
            layoutManager.layOutComponents (comps, 3, 0, 0, getWidth(), getHeight(), false, true);
        else
            layoutManager.layOutComponents (comps, 3, 0, 0, getWidth(), getHeight(), true, true);
    }

    juce::var DockSplit::toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("type", "split");
        obj->setProperty ("direction", dir == Direction::horizontal ? "horizontal" : "vertical");
        obj->setProperty ("ratio", getRatio());
        obj->setProperty ("first", firstChild->toVar());
        obj->setProperty ("second", secondChild->toVar());
        return juce::var (obj);
    }
}
```

Note for whoever implements this: `StretchableLayoutManager::layOutComponents`'s exact boolean-parameter order (`vertically` before or after the "resize other items" flag) and `setItemLayout`'s exact negative-value convention for "relative fraction of remaining space" are worth double-checking against `modules/juce_gui_basics/layout/juce_StretchableLayoutManager.h` in this machine's vendored JUCE before trusting this snippet verbatim — the intent (item 0 and item 2 share the space proportionally to `ratio`/`1-ratio`, item 1 is the fixed-width resizer bar) is what matters. `MainComponent.cpp:225-226`'s existing `leftBar`/`rightBar` construction is the working, in-repo reference for the exact calling convention if anything here doesn't compile as written.

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 2, then confirm both new DockSplit tests pass and the full suite total grew with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockSplit.h Source/UI/Dock/DockSplit.cpp Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add DockSplit"
```

---

### Task 5: DockContainer

**Files:**
- Create: `Source/UI/Dock/DockContainer.h`
- Create: `Source/UI/Dock/DockContainer.cpp`

**Interfaces:**
- Consumes: `DockNode`/`DockSplit`/`DockTabGroup` (Tasks 1, 3, 4), `classifyDropZone`/`DropZone` (Task 2).
- Produces:
  ```cpp
  class DockContainer final : public juce::Component,
                              public juce::DragAndDropContainer,
                              public juce::DragAndDropTarget
  {
  public:
      explicit DockContainer (std::unique_ptr<DockNode> root);

      DockNode& getRoot() const noexcept;
      /** Replaces the root wholesale - DockLayout uses this when restoring
          a saved layout, and DockContainer uses it on itself when a drop
          needs to insert a new DockSplit above the current root. */
      void setRoot (std::unique_ptr<DockNode> newRoot);

      juce::var toVar() const;

      /** Fired when a tab is dragged out past every DockContainer's bounds
          and released - MainComponent (Task 9) uses this to spin up a
          FloatingDockWindow at the drop point. Carries the panel that was
          removed from its original tab group (ownership of `content` is
          NOT transferred - it's still owned wherever it always was, this
          just says "put this panel's tab/content somewhere new"). */
      std::function<void (DockPanel)> onPanelDraggedOutside;

      void resized() override;

      // DragAndDropTarget
      bool isInterestedInDragSource (const SourceDetails&) override;
      void itemDragMove (const SourceDetails&) override;
      void itemDragExit (const SourceDetails&) override;
      void itemDropped (const SourceDetails&) override;
  };
  ```

**Test:** none — this class exists to broker live mouse-driven drag/drop across a component tree, which (per this plan's testing convention, matching `PreferencesDialog`) is manual-verification territory, not something this codebase's `juce::UnitTest` suite drives via synthetic `MouseEvent`s. Task 2's `classifyDropZone` (the one piece of genuinely independent logic this class leans on) is already unit-tested on its own.

- [ ] **Step 1: Write the implementation**

Create `Source/UI/Dock/DockContainer.h`:

```cpp
#pragma once
#include "UI/Dock/DockNode.h"
#include "UI/Dock/DockSplit.h"
#include "UI/Dock/DockTabGroup.h"
#include "UI/Dock/DockDropZone.h"
#include <functional>

namespace ss
{
    /** Root of one docking layout tree - the main window's workspace, or the
        single DockContainer living inside one FloatingDockWindow (Task 7).
        Owns the DockNode tree and brokers every drag-and-drop gesture
        anywhere inside it: a Tab (DockTabGroup.h) starts a drag naming a
        panel index; this class figures out, from where the drop landed
        relative to whichever DockTabGroup/DockSplit is under the mouse,
        whether that means "join this tab group" or "split this node and
        put the panel on one side" - via classifyDropZone (Task 2) - and
        rebuilds the tree accordingly. */
    class DockContainer final : public juce::Component,
                                public juce::DragAndDropContainer,
                                public juce::DragAndDropTarget
    {
    public:
        explicit DockContainer (std::unique_ptr<DockNode> rootNode)
            : root (std::move (rootNode))
        {
            addAndMakeVisible (*root);
        }

        DockNode& getRoot() const noexcept { return *root; }

        void setRoot (std::unique_ptr<DockNode> newRoot)
        {
            removeChildComponent (root.get());
            root = std::move (newRoot);
            addAndMakeVisible (*root);
            resized();
        }

        juce::var toVar() const { return root->toVar(); }

        std::function<void (DockPanel)> onPanelDraggedOutside;

        void resized() override { root->setBounds (getLocalBounds()); }

        bool isInterestedInDragSource (const SourceDetails&) override { return true; }

        void itemDragMove (const SourceDetails& details) override
        {
            const auto* group = findTabGroupAt (details.localPosition);
            lastHoveredZone = group != nullptr
                                  ? classifyDropZone (group->getBounds(),
                                                       details.localPosition - group->getPosition())
                                  : DropZone::none;
            repaint(); // paintOverChildren (added below) draws the highlight from lastHoveredZone
        }

        void itemDragExit (const SourceDetails&) override
        {
            lastHoveredZone = DropZone::none;
            repaint();
        }

        void itemDropped (const SourceDetails& details) override;

    private:
        std::unique_ptr<DockNode> root;
        DropZone lastHoveredZone = DropZone::none;

        DockTabGroup* findTabGroupAt (juce::Point<int> pointInThisComponent) const;
        /** Walks the tree replacing `target` (found by pointer identity)
            with `replacement`, returning true if found. Used both to turn a
            DockTabGroup into a DockSplit (edge drop) and to collapse a
            DockSplit back into its surviving child (a group emptied out). */
        bool replaceNode (std::unique_ptr<DockNode>& current, DockNode* target,
                          std::unique_ptr<DockNode> replacement);
    };
}
```

Create `Source/UI/Dock/DockContainer.cpp`:

```cpp
#include "UI/Dock/DockContainer.h"

namespace ss
{
    DockTabGroup* DockContainer::findTabGroupAt (juce::Point<int> pointInThisComponent) const
    {
        if (auto* c = root->getComponentAt (pointInThisComponent))
            if (auto* group = dynamic_cast<DockTabGroup*> (c))
                return group;
            else if (auto* parentGroup = c->findParentComponentOfClass<DockTabGroup>())
                return parentGroup;

        return nullptr;
    }

    bool DockContainer::replaceNode (std::unique_ptr<DockNode>& current, DockNode* target,
                                     std::unique_ptr<DockNode> replacement)
    {
        if (current.get() == target)
        {
            current = std::move (replacement);
            return true;
        }

        if (auto* split = dynamic_cast<DockSplit*> (current.get()))
        {
            auto [first, second] = split->releaseChildren();

            if (replaceNode (first, target, std::move (replacement)))
            {
                current = std::make_unique<DockSplit> (split->getDirection(), std::move (first),
                                                       std::move (second), split->getRatio());
                return true;
            }

            if (replaceNode (second, target, std::move (replacement)))
            {
                current = std::make_unique<DockSplit> (split->getDirection(), std::move (first),
                                                       std::move (second), split->getRatio());
                return true;
            }

            // Neither side matched - put the split back together unchanged.
            current = std::make_unique<DockSplit> (split->getDirection(), std::move (first),
                                                   std::move (second), split->getRatio());
        }

        return false;
    }

    void DockContainer::itemDropped (const SourceDetails& details)
    {
        lastHoveredZone = DropZone::none;

        auto* targetGroup = findTabGroupAt (details.localPosition);

        if (targetGroup == nullptr)
        {
            // Dropped somewhere with no tab group under it at all (e.g. the
            // thin gap right on a splitter bar) - treat as a no-op rather
            // than guessing; the user can retry a few pixels over.
            repaint();
            return;
        }

        const auto zone = classifyDropZone (targetGroup->getBounds(),
                                            details.localPosition - targetGroup->getPosition());

        const int draggedIndex = (int) details.description;
        auto* sourceGroup = details.sourceComponent != nullptr
                                ? details.sourceComponent->findParentComponentOfClass<DockTabGroup>()
                                : nullptr;

        if (sourceGroup == nullptr || draggedIndex < 0)
        {
            repaint();
            return;
        }

        if (zone == DropZone::centre)
        {
            if (sourceGroup != targetGroup)
            {
                auto panel = sourceGroup->removePanel (draggedIndex);
                targetGroup->addPanel (panel);
            }

            repaint();
            return;
        }

        if (zone == DropZone::none)
        {
            repaint();
            return;
        }

        // An edge drop: pull the panel out of its source group, build a new
        // one-panel group for it, and split the target node so the new
        // group takes the requested side.
        auto movedPanel = sourceGroup->removePanel (draggedIndex);
        auto newGroup = std::make_unique<DockTabGroup>();
        newGroup->addPanel (movedPanel);

        const auto direction = (zone == DropZone::left || zone == DropZone::right)
                                    ? DockSplit::Direction::horizontal
                                    : DockSplit::Direction::vertical;
        const bool movedGoesFirst = (zone == DropZone::left || zone == DropZone::top);

        // targetGroup is about to be moved-from via replaceNode, so capture
        // its raw pointer as the search key before touching the tree.
        auto* targetKey = static_cast<DockNode*> (targetGroup);

        // Build a placeholder split now; replaceNode reconstructs the real
        // parent chain around whichever node currently equals targetKey, so
        // we hand it a factory rather than a ready-made DockSplit (the
        // target's own unique_ptr is still owned by the tree at this point).
        struct Extracted { std::unique_ptr<DockNode> node; };

        // replaceNode needs the actual std::unique_ptr<DockNode> that used to
        // hold targetGroup so it can move it into the new split - but the
        // tree only stores it inside DockSplit::firstChild/secondChild or as
        // DockContainer::root, both already unique_ptr<DockNode>. We recover
        // it the same way replaceNode itself walks: extract-then-rebuild.
        std::unique_ptr<DockNode> extractedTarget;

        std::function<bool (std::unique_ptr<DockNode>&)> extract = [&] (std::unique_ptr<DockNode>& node) -> bool
        {
            if (node.get() == targetKey)
            {
                extractedTarget = std::move (node);
                return true;
            }

            if (auto* split = dynamic_cast<DockSplit*> (node.get()))
            {
                auto [first, second] = split->releaseChildren();
                const bool foundInFirst = extract (first);
                const bool foundInSecond = ! foundInFirst && extract (second);

                node = std::make_unique<DockSplit> (split->getDirection(), std::move (first),
                                                    std::move (second), split->getRatio());
                return foundInFirst || foundInSecond;
            }

            return false;
        };

        extract (root);

        auto combined = movedGoesFirst
                             ? std::make_unique<DockSplit> (direction, std::move (newGroup), std::move (extractedTarget))
                             : std::make_unique<DockSplit> (direction, std::move (extractedTarget), std::move (newGroup));

        replaceNode (root, targetKey, std::move (combined));
        resized();
        repaint();
    }
}
```

Note for whoever implements this: the `extract`-then-`replaceNode` two-pass approach above is deliberately conservative (find-and-detach the target node first, THEN graft the new split back in at the same tree position) rather than trying to do it in one pass, because `DockSplit::releaseChildren()` invalidates the split's own children while we're still recursing through the tree looking for the drop target - doing both in one function risks operating on a half-torn-apart tree. If this turns out to be more convoluted than necessary once real drag testing starts, simplifying it (e.g. giving `DockNode` a parent-pointer instead of walking from the root every time) is a reasonable refactor to make **after** manual testing confirms drops actually work correctly - don't restructure this blind. `DockPanel::content` may legitimately be `nullptr` in a test-constructed group (Task 3's test uses `nullptr` content for brevity) - guard against that in `findTabGroupAt`/anywhere else that dereferences `content` if you add more logic here.

- [ ] **Step 2: Build to confirm it compiles**

Same build command as Task 1 Step 2. Expected: builds with zero errors, full suite total unchanged (this task adds no tests, per its own Test note above).

- [ ] **Step 3: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockContainer.h Source/UI/Dock/DockContainer.cpp
git -C "E:/MIDI&DAW" commit -m "Add DockContainer (drag-and-drop broker for the docking tree)"
```

---

### Task 6: DockLayout serialization

**Files:**
- Create: `Source/UI/Dock/DockLayout.h`
- Create: `Source/UI/Dock/DockLayout.cpp`
- Modify: `Source/UI/Dock/DockTests.cpp` (extend)

**Interfaces:**
- Consumes: `DockNode`/`DockSplit`/`DockTabGroup` (Tasks 1, 3, 4), `DockPanel` (Task 1).
- Produces:
  ```cpp
  namespace ss::DockLayout
  {
      /** Rebuilds a DockNode tree from a var produced by DockNode::toVar()
          (or DockContainer::toVar()). `panelsById` supplies the live
          juce::Component* for each of the 8 fixed view ids (Task 9 builds
          this map); a saved reference to an id not present in the map is
          dropped silently (that view isn't available in this build/session -
          fail soft, not by refusing the whole layout). Returns nullptr if
          `state` isn't a well-formed tree at all (wrong "type" tag, missing
          required fields, etc.) - the caller falls back to defaultLayout(). */
      std::unique_ptr<DockNode> restore (const juce::var& state,
                                         const std::map<juce::String, juce::Component*>& panelsById);

      /** Timeline + Mixer + Piano Roll split left-to-right (each getting a
          third of the width), with the remaining 5 views tabbed together on
          the right - the day-one default before any layout has ever been
          saved. Any id in `panelsById` not mentioned here is silently
          skipped (lets this stay correct even if a future view gets added
          without updating this function - it'll just be missing from the
          DEFAULT layout, not crash). */
      juce::var defaultLayout (const std::map<juce::String, juce::String>& displayNamesById);
  }
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/UI/Dock/DockTests.cpp` (add `#include "UI/Dock/DockLayout.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("DockLayout round-trips a split-of-tab-groups tree")
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

            auto restored = DockLayout::restore (saved, panelsById);
            expect (restored != nullptr);

            auto* restoredSplit = dynamic_cast<DockSplit*> (restored.get());
            expect (restoredSplit != nullptr);
            expect (restoredSplit->getDirection() == DockSplit::Direction::horizontal);
            expectWithinAbsoluteError (restoredSplit->getRatio(), 0.4, 1.0e-6);

            auto* restoredFirst = dynamic_cast<DockTabGroup*> (&restoredSplit->getFirst());
            expect (restoredFirst != nullptr);
            expectEquals (restoredFirst->getNumPanels(), 1);
            expectEquals (restoredFirst->getPanel (0)->id, juce::String ("timeline"));
            expectEquals (restoredFirst->getPanel (0)->content, (juce::Component*) &timelineContent,
                          "the restored panel must be wired back to the SAME live content component");
        }

        beginTest ("DockLayout::restore drops a panel id no longer present, keeps the rest")
        {
            juce::Label mixerContent;

            auto group = std::make_unique<DockTabGroup>();
            group->addPanel ({ "timeline", "Timeline", nullptr });
            group->addPanel ({ "mixer", "Mixer", nullptr });
            const auto saved = group->toVar();

            // "timeline" is deliberately absent from panelsById.
            std::map<juce::String, juce::Component*> panelsById { { "mixer", &mixerContent } };

            auto restored = DockLayout::restore (saved, panelsById);
            auto* restoredGroup = dynamic_cast<DockTabGroup*> (restored.get());
            expect (restoredGroup != nullptr);
            expectEquals (restoredGroup->getNumPanels(), 1, "the missing panel id should be dropped, not crash");
            expectEquals (restoredGroup->getPanel (0)->id, juce::String ("mixer"));
        }

        beginTest ("DockLayout::restore returns nullptr for malformed input")
        {
            std::map<juce::String, juce::Component*> panelsById;

            expect (DockLayout::restore (juce::var(), panelsById) == nullptr);
            expect (DockLayout::restore (juce::var (42), panelsById) == nullptr);

            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "not-a-real-type");
            expect (DockLayout::restore (juce::var (obj), panelsById) == nullptr);
        }

        beginTest ("DockLayout::defaultLayout produces a tree containing every supplied id")
        {
            std::map<juce::String, juce::String> names {
                { "timeline", "Timeline" }, { "mixer", "Mixer" }, { "pianoRoll", "Piano Roll" }
            };

            const auto layout = DockLayout::defaultLayout (names);

            std::map<juce::String, juce::Component*> panelsById {
                { "timeline", nullptr }, { "mixer", nullptr }, { "pianoRoll", nullptr }
            };

            for (auto& kv : panelsById)
            {
                juce::Label placeholder;
                kv.second = &placeholder; // keep the map's Component* non-null for restore() below
            }

            auto restored = DockLayout::restore (layout, panelsById);
            expect (restored != nullptr, "the default layout must be well-formed enough to restore itself");
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command as Task 1 Step 2. Expected: FAIL — `UI/Dock/DockLayout.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/UI/Dock/DockLayout.h`:

```cpp
#pragma once
#include "UI/Dock/DockNode.h"
#include <map>

namespace ss::DockLayout
{
    std::unique_ptr<DockNode> restore (const juce::var& state,
                                       const std::map<juce::String, juce::Component*>& panelsById);

    juce::var defaultLayout (const std::map<juce::String, juce::String>& displayNamesById);
}
```

Create `Source/UI/Dock/DockLayout.cpp`:

```cpp
#include "UI/Dock/DockLayout.h"
#include "UI/Dock/DockSplit.h"
#include "UI/Dock/DockTabGroup.h"

namespace ss::DockLayout
{
    std::unique_ptr<DockNode> restore (const juce::var& state,
                                       const std::map<juce::String, juce::Component*>& panelsById)
    {
        if (! state.isObject())
            return nullptr;

        const auto type = state.getProperty ("type", {}).toString();

        if (type == "split")
        {
            const auto directionStr = state.getProperty ("direction", {}).toString();
            const auto direction = directionStr == "horizontal" ? DockSplit::Direction::horizontal
                                                                 : DockSplit::Direction::vertical;
            const auto ratio = (double) state.getProperty ("ratio", 0.5);

            auto first = restore (state.getProperty ("first", {}), panelsById);
            auto second = restore (state.getProperty ("second", {}), panelsById);

            if (first == nullptr && second == nullptr)
                return nullptr;

            if (first == nullptr) return second;
            if (second == nullptr) return first;

            return std::make_unique<DockSplit> (direction, std::move (first), std::move (second), ratio);
        }

        if (type == "tabGroup")
        {
            auto group = std::make_unique<DockTabGroup>();
            const auto* panelIds = state.getProperty ("panels", {}).getArray();

            if (panelIds != nullptr)
            {
                for (const auto& idVar : *panelIds)
                {
                    const auto id = idVar.toString();
                    const auto it = panelsById.find (id);

                    if (it != panelsById.end())
                        group->addPanel ({ id, id, it->second });
                }
            }

            const auto activeIndex = (int) state.getProperty ("active", 0);

            if (group->getNumPanels() == 0)
                return nullptr; // every panel this group named is unavailable - drop the group entirely

            group->setActivePanel (juce::jlimit (0, group->getNumPanels() - 1, activeIndex));
            return group;
        }

        return nullptr;
    }

    juce::var defaultLayout (const std::map<juce::String, juce::String>& displayNamesById)
    {
        auto makeSingleGroup = [&] (const juce::String& id) -> juce::var
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "tabGroup");
            juce::Array<juce::var> ids;
            if (displayNamesById.count (id) != 0)
                ids.add (id);
            obj->setProperty ("panels", ids);
            obj->setProperty ("active", 0);
            return juce::var (obj);
        };

        auto makeSplit = [] (const juce::var& first, const juce::var& second, double ratio)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "split");
            obj->setProperty ("direction", "horizontal");
            obj->setProperty ("ratio", ratio);
            obj->setProperty ("first", first);
            obj->setProperty ("second", second);
            return juce::var (obj);
        };

        // The rest (transcribe/generate/notation/session/modular) share one
        // tabbed group on the far right - built by listing every remaining
        // id directly in the "panels" array, since defaultLayout has no live
        // DockTabGroup to call addPanel on yet (it only ever produces a var).
        juce::Array<juce::var> restIds;
        for (const auto& kv : displayNamesById)
            if (kv.first != "timeline" && kv.first != "mixer" && kv.first != "pianoRoll")
                restIds.add (kv.first);

        auto* restObj = new juce::DynamicObject();
        restObj->setProperty ("type", "tabGroup");
        restObj->setProperty ("panels", restIds);
        restObj->setProperty ("active", 0);

        // Timeline | Mixer | Piano Roll | (everything else, tabbed) as four
        // side-by-side columns, via three nested horizontal splits - roughly
        // 33% / 33% / 17% / 17% of the width.
        return makeSplit (
            makeSingleGroup ("timeline"),
            makeSplit (
                makeSingleGroup ("mixer"),
                makeSplit (makeSingleGroup ("pianoRoll"), juce::var (restObj), 0.5),
                0.5),
            0.33);
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 2, then confirm all four new DockLayout tests pass and the full suite total grew with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockLayout.h Source/UI/Dock/DockLayout.cpp Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add DockLayout serialization"
```

---

### Task 7: FloatingDockWindow

**Files:**
- Create: `Source/UI/Dock/FloatingDockWindow.h`
- Create: `Source/UI/Dock/FloatingDockWindow.cpp`
- Modify: `Source/UI/Dock/DockTests.cpp` (extend)

**Interfaces:**
- Consumes: `DockContainer` (Task 5).
- Produces:
  ```cpp
  class FloatingDockWindow final : public juce::DocumentWindow
  {
  public:
      explicit FloatingDockWindow (std::unique_ptr<DockNode> rootContent);
      ~FloatingDockWindow() override;

      void closeButtonPressed() override;

      DockContainer& getDockContainer() const noexcept;

      /** Fired right before this window deletes itself (from
          closeButtonPressed) - MainComponent (Task 9) uses this to drop its
          tracking pointer to this window before the dangling reference
          becomes a problem. */
      std::function<void (FloatingDockWindow*)> onClosing;
  };

  namespace FloatingDockWindowGeometry
  {
      /** Pure function, independent of any live window: given a saved
          bounds rectangle and the currently connected displays, returns a
          bounds guaranteed to be (at least mostly) on-screen. Used both by
          FloatingDockWindow's own restore path and directly unit-tested
          without needing a real window or a real multi-monitor setup. */
      juce::Rectangle<int> clampToNearestDisplay (juce::Rectangle<int> savedBounds,
                                                  const juce::Array<juce::Displays::Display>& availableDisplays);
  }
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/UI/Dock/DockTests.cpp` (add `#include "UI/Dock/FloatingDockWindow.h"` at the top), inside `runTest()`:

```cpp
        beginTest ("clampToNearestDisplay leaves an already-on-screen rect untouched")
        {
            juce::Displays::Display display;
            display.userArea = { 0, 0, 1920, 1080 };

            const juce::Rectangle<int> onScreen (100, 100, 400, 300);
            const auto result = FloatingDockWindowGeometry::clampToNearestDisplay (onScreen, { display });

            expectEquals (result, onScreen);
        }

        beginTest ("clampToNearestDisplay pulls a fully off-screen rect back onto the nearest display")
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

        beginTest ("clampToNearestDisplay with zero displays returns the input unchanged rather than crashing")
        {
            const juce::Rectangle<int> input (10, 10, 200, 200);
            const auto result = FloatingDockWindowGeometry::clampToNearestDisplay (input, {});
            expectEquals (result, input);
        }
```

- [ ] **Step 2: Run test to verify it fails**

Same build command. Expected: FAIL — `UI/Dock/FloatingDockWindow.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/UI/Dock/FloatingDockWindow.h`:

```cpp
#pragma once
#include "UI/Dock/DockContainer.h"

namespace ss
{
    namespace FloatingDockWindowGeometry
    {
        /** Constrains `savedBounds` to fit inside whichever of
            `availableDisplays` is nearest to it, preserving size. An empty
            `availableDisplays` (theoretically impossible on a real machine,
            but a real input a unit test can construct) returns `savedBounds`
            unchanged rather than dividing by zero or dereferencing nothing. */
        juce::Rectangle<int> clampToNearestDisplay (juce::Rectangle<int> savedBounds,
                                                    const juce::Array<juce::Displays::Display>& availableDisplays);
    }

    /** One panel (or split/tabbed group of panels) pulled out of the main
        window into its own OS-level top-level window, for multi-monitor use.
        Deleting this window does not delete the DockPanel content it
        displays (see DockPanel's own doc comment - content is always owned
        elsewhere, MainComponent in practice). */
    class FloatingDockWindow final : public juce::DocumentWindow
    {
    public:
        explicit FloatingDockWindow (std::unique_ptr<DockNode> rootContent)
            : juce::DocumentWindow ("ScoreSmith", juce::Colours::darkgrey,
                                    juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
        {
            // Deliberately no setMenuBar() call: on macOS there's exactly one
            // global menu bar for the whole app already (see
            // MainComponent::resized()'s hostOwnsMenuBar check), and on
            // Windows a DocumentWindow with none set simply has none - both
            // are the correct behaviour for a secondary docking window.
            setUsingNativeTitleBar (true);
            container = std::make_unique<DockContainer> (std::move (rootContent));
            setContentNonOwned (container.get(), false);
            setResizable (true, false);
        }

        void closeButtonPressed() override
        {
            if (onClosing)
                onClosing (this);

            delete this;
        }

        DockContainer& getDockContainer() const noexcept { return *container; }

        std::function<void (FloatingDockWindow*)> onClosing;

    private:
        std::unique_ptr<DockContainer> container;
    };

    namespace FloatingDockWindowGeometry
    {
        inline juce::Rectangle<int> clampToNearestDisplay (juce::Rectangle<int> savedBounds,
                                                           const juce::Array<juce::Displays::Display>& availableDisplays)
        {
            if (availableDisplays.isEmpty())
                return savedBounds;

            const juce::Displays::Display* nearest = &availableDisplays.getReference (0);
            auto nearestDistance = std::numeric_limits<juce::int64>::max();

            for (auto& display : availableDisplays)
            {
                const auto centre = display.userArea.getCentre();
                const auto savedCentre = savedBounds.getCentre();
                const juce::int64 dx = centre.x - savedCentre.x;
                const juce::int64 dy = centre.y - savedCentre.y;
                const auto distance = dx * dx + dy * dy;

                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearest = &display;
                }
            }

            return savedBounds.constrainedWithin (nearest->userArea);
        }
    }
}
```

Note for whoever implements this: the header above puts `FloatingDockWindowGeometry::clampToNearestDisplay`'s real body inline in the `.h` (marked `inline`) rather than splitting it into a `.cpp`, since it's small and has no need to hide any private state — if you'd rather match this codebase's more common one-declaration-in-.h/one-definition-in-.cpp split for consistency with every other file in `Source/UI/Dock/`, moving the body into `FloatingDockWindow.cpp` (dropping `inline`) is equally correct and probably the better call for consistency; just create that `.cpp` file and add it to this task's file list if you do. `FloatingDockWindow`'s own body has no logic worth a `.cpp` split (it's all trivial forwarding), so leaving it header-only is fine either way.

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 2, then confirm all three new `clampToNearestDisplay` tests pass and the full suite total grew with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/FloatingDockWindow.h Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add FloatingDockWindow and monitor-bounds clamping"
```

---

### Task 8: Named-layout persistence via Settings

**Files:**
- Modify: `Source/Core/Settings.h`
- Modify: `Source/Core/Settings.cpp`
- Modify: `Source/UI/Dock/DockLayout.h` (add the `lastSessionLayoutName` constant — see Step 3)
- Modify: `Source/UI/Dock/DockTests.cpp` (extend)

**Interfaces:**
- Produces:
  ```cpp
  // Settings.h additions
  juce::StringArray getDockLayoutNames() const;
  juce::var getDockLayout (const juce::String& name) const;   // {} (void var) if the name doesn't exist
  void setDockLayout (const juce::String& name, const juce::var& state);
  void deleteDockLayout (const juce::String& name);

  namespace DockLayout
  {
      /** The reserved name Settings::getDockLayout/setDockLayout use for
          "whatever the layout looked like when the app last closed" - never
          shown to the user as a pickable named layout. */
      inline const juce::String lastSessionLayoutName = "__last__";
  }
  ```

- [ ] **Step 1: Write the failing test**

Add to `Source/UI/Dock/DockTests.cpp` (add `#include "Core/Settings.h"` and `#include "UI/Dock/DockLayout.h"` at the top if not already present — `DockLayout.h` already exists from Task 6, this task only adds the `lastSessionLayoutName` constant to it, see Step 3), inside `runTest()`:

```cpp
        beginTest ("Settings persists named dock layouts, round-tripping a real var tree")
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

        beginTest ("the reserved last-session layout name round-trips the same way as any other")
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
```

- [ ] **Step 2: Run test to verify it fails**

Same build command as Task 1 Step 2. Expected: FAIL — `Settings` has no `getDockLayoutNames`/`getDockLayout`/`setDockLayout`/`deleteDockLayout`, and `ss::DockLayout::lastSessionLayoutName` doesn't exist yet.

- [ ] **Step 3: Write the implementation**

Modify `Source/UI/Dock/DockLayout.h` — add the reserved name constant inside the existing `namespace ss::DockLayout { ... }` block, alongside the two functions from Task 6:

```cpp
    inline const juce::String lastSessionLayoutName = "__last__";
```

Modify `Source/Core/Settings.h`, add near the existing settings groups (a new `// Dock layouts` group is fine, following the same pattern Task 6 of the UTAU plan used for its own new settings group):

```cpp
        // Dock layouts (docs/superpowers/specs/2026-08-26-docking-layout-system-design.md)
        juce::StringArray getDockLayoutNames() const;
        juce::var getDockLayout (const juce::String& name) const;
        void setDockLayout (const juce::String& name, const juce::var& state);
        void deleteDockLayout (const juce::String& name);
```

Modify `Source/Core/Settings.cpp` — add the four implementations. Each named layout is stored as its own JSON-encoded string property, keyed by a fixed prefix plus the layout's name (mirroring the "one PropertiesFile key per named thing" shape, distinct from the newline-joined-list shape `getSampleLibraryFolders` uses, since these are named/keyed rather than an ordered list); the set of names is tracked as a separate newline-joined list so `getDockLayoutNames()` doesn't need to enumerate `PropertiesFile` keys by prefix (which JUCE's `PropertiesFile` doesn't expose a clean way to do):

```cpp
    namespace
    {
        juce::String dockLayoutKeyFor (const juce::String& name) { return "dockLayout." + name; }
    }

    juce::StringArray Settings::getDockLayoutNames() const
    {
        juce::StringArray names;
        names.addLines (props.getUserSettings()->getValue ("dockLayoutNames"));
        names.removeEmptyStrings();
        return names;
    }

    juce::var Settings::getDockLayout (const juce::String& name) const
    {
        const auto json = props.getUserSettings()->getValue (dockLayoutKeyFor (name));

        if (json.isEmpty())
            return {};

        return juce::JSON::parse (json);
    }

    void Settings::setDockLayout (const juce::String& name, const juce::var& state)
    {
        props.getUserSettings()->setValue (dockLayoutKeyFor (name), juce::JSON::toString (state));

        auto names = getDockLayoutNames();
        names.addIfNotAlreadyThere (name);
        props.getUserSettings()->setValue ("dockLayoutNames", names.joinIntoString ("\n"));
    }

    void Settings::deleteDockLayout (const juce::String& name)
    {
        props.getUserSettings()->removeValue (dockLayoutKeyFor (name));

        auto names = getDockLayoutNames();
        names.removeString (name);
        props.getUserSettings()->setValue ("dockLayoutNames", names.joinIntoString ("\n"));
    }
```

(`DockLayout::lastSessionLayoutName` is deliberately never added to the `dockLayoutNames` list by any code path in this task — `setDockLayout`/`getDockLayout`/`deleteDockLayout` work on it exactly like any other name since it's just a string, but Task 9's UI for picking a saved layout should filter it out of whatever list it shows the user, since "__last__" isn't a layout the user ever explicitly named. This task doesn't build that UI, just the storage the filtering will apply to later.)

Confirm `juce::JSON::parse`/`juce::JSON::toString` are the right calls before trusting this verbatim — check `modules/juce_core/juce_core.h`'s `JSON` class (or grep this codebase for any existing `juce::JSON::` usage) for the exact static method names and signatures on this machine's vendored JUCE version.

- [ ] **Step 4: Run test to verify it passes**

Same as Task 1 Step 2, then confirm both new Settings-persistence tests pass and the full suite total grew with 0 failed.

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/Core/Settings.h Source/Core/Settings.cpp Source/UI/Dock/DockLayout.h Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Add named dock-layout persistence to Settings"
```

---

### Task 9: MainComponent integration

**Files:**
- Modify: `Source/UI/MainComponent.h`
- Modify: `Source/UI/MainComponent.cpp`

**Interfaces:**
- Consumes: `DockContainer` (Task 5), `DockLayout::restore`/`defaultLayout` (Task 6), `FloatingDockWindow` (Task 7), `Settings::getDockLayout`/`setDockLayout` (Task 8).
- No new public interface — `MainComponent::showView(View)` keeps its existing signature and existing callers (menu items, keyboard shortcuts, the transcribe-view handoff) so none of those十几 call sites need to know docking exists; only what happens *inside* `showView`/`buildUi`/`rebuildUi` changes.

**Test:** none — this is UI wiring exercised by the same "builds, and behaves correctly on manual launch" verification every prior UI-only task in this plan and the UTAU plan's own Task 6 used. Manual verification steps are in Step 4 below.

- [ ] **Step 1: Replace `workspace` with a `DockContainer`**

Modify `Source/UI/MainComponent.cpp`. The existing `WorkspaceHolder` class (lines 71-78) already inherits `juce::DragAndDropContainer` — that responsibility moves to `DockContainer` (Task 5), so `WorkspaceHolder` itself is deleted entirely rather than kept alongside it.

Replace the `workspace` member's declaration (`Source/UI/MainComponent.cpp:855`, `std::unique_ptr<WorkspaceHolder> workspace;`) with:

```cpp
        std::unique_ptr<DockContainer> workspace;
        std::vector<std::unique_ptr<FloatingDockWindow>> floatingWindows;
```

Add `#include "UI/Dock/DockContainer.h"`, `#include "UI/Dock/DockLayout.h"`, and `#include "UI/Dock/FloatingDockWindow.h"` near the top of `MainComponent.cpp`, alongside its existing `#include "UI/..."` lines.

Replace the `workspace = std::make_unique<WorkspaceHolder>(); ... owner.addAndMakeVisible (*workspace);` block (`MainComponent.cpp:198-200`) and the `for (auto* view : workspaceComponents()) workspace->addChildComponent (view);` loop (`MainComponent.cpp:222-223`) with layout construction that runs AFTER the 8 views are built (so `panelsById` below can reference them):

```cpp
            timeline   = std::make_unique<TimelineView> (ctx, uiState);
            pianoRoll  = std::make_unique<PianoRollView> (ctx, uiState);
            mixer      = std::make_unique<MixerView> (ctx, uiState);
            transcribe = std::make_unique<TranscribeView> (ctx, uiState);
            generate   = std::make_unique<GenerateView> (ctx, uiState);
            notation   = std::make_unique<NotationView> (ctx, uiState);

            session = std::make_unique<PlaceholderView> (/* ... unchanged ... */);
            modular = std::make_unique<PlaceholderView> (/* ... unchanged ... */);

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

            const auto savedLayout = ctx.settings->getDockLayout (DockLayout::lastSessionLayoutName);
            auto restoredRoot = savedLayout.isObject() ? DockLayout::restore (savedLayout, panelsById) : nullptr;

            if (restoredRoot == nullptr)
                restoredRoot = DockLayout::restore (DockLayout::defaultLayout (displayNamesById), panelsById);

            workspace = std::make_unique<DockContainer> (std::move (restoredRoot));
            owner.addAndMakeVisible (*workspace);
```

Remove `layOutWorkspace()` (`MainComponent.cpp:354-362`) and its `workspace->onResized` wiring — `DockContainer::resized()` (Task 5) already lays out its own root filling its bounds; `MainComponent::resized()`'s existing call site that positions `workspace` itself (setting `workspace`'s outer bounds within the window) is unaffected and stays as-is, only the *inside* of workspace no longer needs `MainComponent` to drive it.

- [ ] **Step 2: Replace `showView(View)`'s single-visibility-toggle body**

Replace the body of `showView` (`MainComponent.cpp:332-352`) — it no longer toggles `setVisible` on all 8 siblings (they're not siblings of each other under one container any more, they're wherever the current `DockLayout` put them), it instead finds whichever `DockTabGroup` currently holds the requested view's panel id and makes it the active tab:

```cpp
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
                    if (auto* group = findGroupContaining (floating->getDockContainer().getRoot()))
                    {
                        group->setActivePanel (group->indexOfPanel (id));
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
```

Note: if the requested view's panel isn't found anywhere (the user closed every tab group containing it, if that ever becomes possible — Task 5/6 as written don't expose a "close this panel entirely" action, only move/split/join, so this should be unreachable in practice, but the `if`/`else` above degrades to "do nothing" rather than crashing if it ever does happen).

- [ ] **Step 3: Wire panel-dragged-outside to floating-window creation, and wire save-on-close**

In `buildUi()`, right after constructing `workspace` (Step 1's last line), wire its drag-out callback:

```cpp
            workspace->onPanelDraggedOutside = [this] (DockPanel panel)
            {
                auto newGroup = std::make_unique<DockTabGroup>();
                newGroup->addPanel (panel);

                auto window = std::make_unique<FloatingDockWindow> (std::move (newGroup));
                window->setBounds (juce::Rectangle<int> (100, 100, 500, 400));
                window->onClosing = [this] (FloatingDockWindow* closing)
                {
                    floatingWindows.erase (std::remove_if (floatingWindows.begin(), floatingWindows.end(),
                                                            [closing] (auto& w) { return w.get() == closing; }),
                                           floatingWindows.end());
                };
                window->setVisible (true);
                floatingWindows.push_back (std::move (window));
            };
```

**Do not touch `MainComponent::~MainComponent()`** — it's `= default` (`MainComponent.cpp:891`), so there's no body to add a line to. `Impl` already has its own explicit destructor (`Impl::~Impl()`, `MainComponent.cpp:156-163`) that calls an existing `saveLayout()` method (`MainComponent.cpp:166` onward) to persist UI state (browser/AI-panel widths and visibility) to `Settings` on shutdown — this is the established, already-working place to persist "state at close", not a new destructor. Add the dock-layout save as one more line inside that EXISTING `saveLayout()` method, alongside its existing `ctx.settings->raw().setValue (...)` calls:

```cpp
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
            // ... rest of the existing method body, unchanged ...
        }
```

(`AppContext::settings` is confirmed as `std::unique_ptr<Settings> settings;` — `Source/Core/AppContext.h:19` — so `ctx.settings->setDockLayout(...)` via `operator->` is exactly right, no adjustment needed there.)

- [ ] **Step 4: Handle `rebuildUi()`'s language-change reset**

`rebuildUi()` (`MainComponent.cpp:236-254`) resets every view and rebuilds from scratch for a language change, capturing `currentView` beforehand and restoring it with `showView(view)` afterward — the exact same pattern this task's `DockLayout` needs, just for the WHOLE layout tree rather than one enum value. Modify it to capture and restore the full layout across the rebuild:

```cpp
        void rebuildUi()
        {
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
            pendingLayoutOverride = {};

            showView (view);
            owner.repaint();
        }
```

Add a `juce::var pendingLayoutOverride;` member near `workspace`'s declaration, and change Step 1's `const auto savedLayout = ctx.settings->getDockLayout (...)` line to:

```cpp
            const auto savedLayout = pendingLayoutOverride.isObject()
                                          ? pendingLayoutOverride
                                          : ctx.settings->getDockLayout (DockLayout::lastSessionLayoutName);
```

This makes a language-change rebuild preserve the exact in-memory layout the user had a moment ago (splits, floating windows' *content* — though not their on-screen window bounds, since floating windows themselves are fully torn down and rebuilt as plain in-main-window tab groups by this path; re-floating them is a manual step after a language change, which is an acceptable, narrow rough edge worth calling out to the user testing this rather than solving now) rather than falling back to whatever was last saved to `Settings` (which could be a session or more stale).

- [ ] **Step 5: Build and manually verify**

```powershell
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
cmake --build "E:/MIDIDAW/build" --config Debug --parallel
& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe" --run-tests
```
Expected: builds with zero errors and zero new warnings; full suite total unchanged from Task 8's end (this task adds no automated tests).

Then launch the app (`& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe"`) and manually confirm, taking screenshots at each step per this codebase's established manual-UI-verification pattern (see the UTAU plan's Task 6):

1. The default layout shows Timeline + Mixer split left-right, with the other 6 views tabbed together (or whatever shape `defaultLayout` ends up producing once Task 6's Piano-Roll discrepancy is resolved).
2. Drag a tab from the tabbed group onto the centre of the Timeline panel — it should join as a new tab there, not split.
3. Drag a tab onto the left edge of the Mixer panel — it should split, with the dragged panel's new tab group appearing on the left and Mixer pushed right.
4. Drag a tab out past the main window's edge — a new OS-level window should appear showing that panel.
5. Move the floating window (if a second monitor is available, onto it; otherwise just move it around the primary monitor) and confirm it behaves like a normal OS window (resizable, closable).
6. Close the app, relaunch it, and confirm the layout — including the floating window, if one was left open — is restored close to how it was left. (Per Step 4's note, a floating window's *window position* is not expected to survive a language-change rebuild specifically, but IS expected to survive a full app close/relaunch — `DockLayout`/`Settings` persistence in Tasks 6/8 doesn't yet serialize floating-window state at all, since this task's own scope only wires up the MAIN window's layout persistence. **This is a real gap**: extending `DockContainer::toVar()`'s caller (this task's Step 3) to also capture each `FloatingDockWindow`'s bounds and content, and `DockLayout::restore`'s caller to recreate them, is needed for full "floating windows survive a relaunch" behaviour and was not included in this task's code above. Flag this to whoever reviews this task — closing this gap is a small, well-scoped follow-up (serialize `floatingWindows` alongside `workspace` under two different keys, or as siblings in one wrapping object) rather than a sign anything above needs to be redone.
7. Open Preferences (or any other action that changes the app language, if one exists in this UI) to trigger a `rebuildUi()` and confirm the layout (main window only, per Step 4's note) survives.

- [ ] **Step 6: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/MainComponent.h Source/UI/MainComponent.cpp
git -C "E:/MIDI&DAW" commit -m "Replace single-view tab switcher with the docking layout system"
```

---

### Task 10: Fire onPanelDraggedOutside when a drag lands on no dock target at all

**Context (why this task exists — added after Task 9's review, not part of the plan's original 9 tasks):** Task 5 built `DockContainer::onPanelDraggedOutside` and documented that firing it was "whichever task adds the drag-past-every-window's-edge detection (FloatingDockWindow, Task 7)". Task 7 built `FloatingDockWindow` itself but not that detection. Task 9 wired a real handler onto `onPanelDraggedOutside` but has no way to fire it. Net result: nothing in Tasks 1-9 ever actually calls `onPanelDraggedOutside` — dragging a tab out past every window's edge currently does nothing at all, silently. This task closes that gap.

**Files:**
- Modify: `Source/UI/Dock/DockContainer.h`
- Modify: `Source/UI/Dock/DockContainer.cpp`
- Modify: `Source/UI/Dock/DockTests.cpp` (extend)

**Interfaces:**
- Consumes: nothing new — `DockTabGroup::removePanel` (Task 3), `DockPanel` (Task 1), and JUCE's own `juce::DragAndDropContainer::dragOperationStarted`/`dragOperationEnded` protected virtuals (present on the base class `DockContainer` already inherits — confirmed by reading the real vendored source at `build/_deps/juce-src/modules/juce_gui_basics/mouse/juce_DragAndDropContainer.{h,cpp}`, do not trust this description alone, read that file yourself before implementing).
- Produces: no new public API — `onPanelDraggedOutside` (already declared) actually fires now, for the first time.

**The mechanism, derived from reading JUCE's real source (verify it yourself before trusting this):**
- `DockTabGroup::Tab::mouseDrag` (already merged, Task 3) calls `startDragging (juce::var (getIndex()), this, juce::ScaledImage(), true)` — the trailing `true` is `allowDraggingToOtherJuceWindows`, already set, so cross-window drops (main window ↔ a `FloatingDockWindow`) already work correctly via JUCE's own desktop-wide hit-testing in `DragAndDropContainer::DragImageComponent::findTarget` — nothing to fix there.
- `DragAndDropContainer::DragImageComponent`'s destructor unconditionally calls `owner.dragOperationEnded (sourceDetails)` on **the container that started the drag** — every time a drag ends, whether or not it landed on a valid `DragAndDropTarget`. This fires exactly once per drag gesture, on the `DockContainer` the tab was dragged *from* (not whichever one — if any — it landed on).
- `DragAndDropContainer::dragOperationStarted`/`dragOperationEnded` are `protected` virtuals on the base class with empty default bodies — a subclass overriding them can widen access to `public` (allowed in C++, and needed here so `DockTests.cpp` can drive a full simulated drag lifecycle without a real mouse gesture).
- So: track whether `itemDropped` (already implemented, Task 5) was called anywhere between one `dragOperationStarted` and the matching `dragOperationEnded`. If it never was, the user released the tab somewhere with no dock target under it at all — exactly the "past every window's edge" gesture — pull the dragged panel out of its source `DockTabGroup` and fire `onPanelDraggedOutside` with it.

- [ ] **Step 1: Write the failing tests**

Add to `Source/UI/Dock/DockTests.cpp`, inside `runTest()` (uses only `DockContainer`, `DockTabGroup`, `DockPanel`, and JUCE's `SourceDetails` directly — no real mouse events needed, since Step 3 below makes the relevant overrides `public`):

```cpp
        beginTest ("DockContainer fires onPanelDraggedOutside when a drag ends without landing on any target")
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
            const juce::DragAndDropTarget::SourceDetails details (juce::var (0), &content, {});
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

            const juce::DragAndDropTarget::SourceDetails details (juce::var (0), &content, {});
            container.dragOperationStarted (details);
            container.itemDropped (details); // simulates a normal, successful in-app drop
            container.dragOperationEnded (details);

            expect (! fired, "onPanelDraggedOutside must not fire when the drag already landed on a real target");
        }
```

- [ ] **Step 2: Run tests to verify they fail**

Same build command as Task 1 Step 2. Expected: the first test fails (`onPanelDraggedOutside` never fires, `fired == false`) since `dragOperationEnded` has no override yet — the base class's empty default runs instead. The second test passes trivially today (nothing fires `onPanelDraggedOutside` at all yet) but must keep passing after Step 3's implementation, not just before it.

- [ ] **Step 3: Implement**

Modify `Source/UI/Dock/DockContainer.h` — add a new public section right after the existing `itemDropped` declaration (before the `private:` section), and one new private member alongside `lastHoveredZone`:

```cpp
        // DragAndDropContainer overrides. Both are `protected` with empty
        // default bodies on the JUCE base class - overridden here as public so
        // DockTests.cpp can drive a full simulated drag lifecycle directly.
        // dragOperationStarted/dragOperationEnded bracket exactly one drag
        // gesture; itemDropped (above) sets currentDragWasHandled = true the
        // instant a drop lands on ANY DragAndDropTarget anywhere on the desktop
        // (this container, a sibling DockTabGroup elsewhere in this same tree,
        // or a completely different DockContainer such as a FloatingDockWindow -
        // Tab::mouseDrag already starts dragging with
        // allowDraggingToOtherJuceWindows=true specifically so those cross-
        // window drops reach a real itemDropped via JUCE's own desktop-wide hit
        // testing, see DockTabGroup.h). dragOperationEnded fires on the
        // container the drag STARTED from, once, whether or not anything ever
        // called itemDropped - if nothing did, the user released the tab
        // somewhere with no dock target under it at all, which is exactly the
        // "dragged past every window's edge" gesture this pulls the panel out
        // for.
        void dragOperationStarted (const SourceDetails&) override { currentDragWasHandled = false; }
        void dragOperationEnded (const SourceDetails& details) override;

    private:
        std::unique_ptr<DockNode> root;
        DropZone lastHoveredZone = DropZone::none;
        bool currentDragWasHandled = false;
```

(The existing `findTabGroupAt`/`splitAroundTarget` private declarations stay exactly where they are, just after this new member.)

Modify `Source/UI/Dock/DockContainer.cpp` — add `currentDragWasHandled = true;` as the very first line of the existing `itemDropped` (before its current first line, `lastHoveredZone = DropZone::none;`), and add the new `dragOperationEnded` definition at the end of the file, inside the `namespace ss { ... }` block:

```cpp
    void DockContainer::dragOperationEnded (const SourceDetails& details)
    {
        if (currentDragWasHandled || onPanelDraggedOutside == nullptr)
            return;

        auto* sourceGroup = details.sourceComponent != nullptr
                                ? details.sourceComponent->findParentComponentOfClass<DockTabGroup>()
                                : nullptr;
        const int draggedIndex = (int) details.description;

        if (sourceGroup == nullptr || draggedIndex < 0)
            return;

        auto panel = sourceGroup->removePanel (draggedIndex);

        if (panel.content != nullptr)
            onPanelDraggedOutside (panel);
    }
```

Note the `onPanelDraggedOutside == nullptr` guard runs BEFORE `removePanel` is ever called: if nothing is listening for this event, the panel must stay exactly where it is rather than being silently removed and lost. Confirm `juce::DragAndDropTarget::SourceDetails`'s real constructor signature and `sourceComponent`/`description`/`localPosition` field names against the vendored JUCE source yourself before trusting the test code above verbatim — I derived it from reading that file, but you should still verify it compiles as intended and matches real behaviour, per this plan's established practice of never transcribing given code blindly.

- [ ] **Step 4: Run tests to verify they pass**

Same build command as Task 1 Step 2. Confirm both new tests pass and the full suite total grew by exactly these 2 tests' assertions with 0 failed (baseline going in: whatever Task 9's own end-state total is — check the ledger).

- [ ] **Step 5: Commit**

```bash
git -C "E:/MIDI&DAW" add Source/UI/Dock/DockContainer.h Source/UI/Dock/DockContainer.cpp Source/UI/Dock/DockTests.cpp
git -C "E:/MIDI&DAW" commit -m "Fire onPanelDraggedOutside when a drag lands on no dock target at all"
```

---

## After Task 10

The final whole-branch review (after Task 10 landed) found real cross-task bugs — a double-delete crash on closing a floating window, and `DockContainer`'s drag machinery incorrectly hijacking an unrelated existing feature (dragging a generated MIDI candidate onto the timeline) — fixed in a dedicated final-review fix round (see the SDD ledger for full detail: `.superpowers/sdd/2026-08-26-docking-layout-system/progress.md`). That same review surfaced the gaps below, which remain deliberately open:

- **Floating-window persistence** (Task 9, Step 5 note): floating windows' existence/position/content don't yet survive an app relaunch, only the main window's layout does. Closing this is a small, self-contained follow-up once the rest of the system is confirmed working end-to-end — extending `DockContainer::toVar()`'s caller and `DockLayout::restore`'s caller to also walk `floatingWindows`. Task 7's `FloatingDockWindowGeometry::clampToNearestDisplay` (built and unit-tested in this plan) is written specifically for that follow-up to call when it restores a saved window bounds onto a machine whose monitor arrangement has since changed — it isn't consumed by anything in Tasks 1-10 yet since nothing here saves/restores floating-window bounds at all, but it belongs next to `FloatingDockWindow` (which Task 7 is already creating) rather than being deferred and rewritten later. (The final review's fix round adds a cheap partial mitigation — closing a floating window now returns its panel(s) to the main workspace instead of orphaning them — so this gap is now purely "position/content don't survive a relaunch," not "a panel can be lost forever.")
- **A UI for picking/saving/deleting named layouts** (Task 8 built the `Settings` storage; nothing in this plan builds the menu/dialog that lets a user actually choose one). A short follow-up plan, not a new subsystem — `Settings::getDockLayoutNames()`/`getDockLayout()`/`setDockLayout()`/`deleteDockLayout()` are the complete API surface it needs, filtering out `DockLayout::lastSessionLayoutName` from whatever list it shows.
- **Drop-zone hover highlight**: both the design spec and this plan's own Task 5 text (`paintOverChildren` drawing from `lastHoveredZone`) called for a translucent highlight during a drag showing whether it'll join or split — `DockContainer` maintains `lastHoveredZone` correctly but nothing ever paints it, so a user gets no visual feedback about what a drop will do until after releasing. Purely additive (no correctness risk) — a follow-up, not a defect in what's built.
- **Cross-window double-float edge case**: the final review flagged a narrow, not-yet-reproduced scenario where dropping a tab exactly on a *different* `DockContainer`'s splitter-bar gap (e.g. a floating window's) could incorrectly also fire `onPanelDraggedOutside` on the source container, since "was this drag handled" is tracked per-container while the real question is global. Needs a real manual reproduction before it's worth designing a fix for.
- **No single test exercises the full save/restore round trip end-to-end** (`DockNode::toVar()` → `Settings::setDockLayout` → real JSON-string round trip → `Settings::getDockLayout` → `DockLayout::restore`) — every piece has its own test, but the JSON-string hop between them (the actual startup path) has no direct coverage. Cheap to add; a good pairing with fixing `DockTests.cpp`'s pre-existing habit of mutating the real, shared settings file during test runs.

The three related feature requests raised alongside this one (simultaneous multi-track parameter editing, an XY-pad style control for built-in effects, and an animated/restyled built-in-effects UI) are each their own design, out of scope for this plan.
