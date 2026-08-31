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

        /** Replaces the root wholesale - DockLayout uses this when restoring
            a saved layout, and DockContainer uses it on itself when a drop
            needs to insert a new DockSplit above the current root. */
        void setRoot (std::unique_ptr<DockNode> newRoot)
        {
            removeChildComponent (root.get());
            root = std::move (newRoot);
            addAndMakeVisible (*root);
            resized();
        }

        juce::var toVar() const { return root->toVar(); }

        /** Strips every panel out of this container's whole tree (walking
            splits, emptying every DockTabGroup it finds) and hands them back
            in depth-first, left-to-right, tab order. Ownership of each
            panel's `content` is unaffected as always (DockPanel never owns
            it) - this just says "these panels no longer live here, put them
            somewhere".

            This leaves the tree structurally intact but completely empty, so
            the caller is expected to DISCARD this DockContainer immediately
            afterwards rather than keep using it. The one real caller does
            exactly that: MainComponent's FloatingDockWindow::onClosing
            handler rescues the closing window's panels into the main
            workspace microseconds before that window (and this container
            with it) is destroyed. */
        std::vector<DockPanel> extractAllPanels();

        /** Splits the whole current tree in two and grafts a brand-new tab
            group holding `panel` onto the right, making it active. Call this
            only once the caller has confirmed `panel.id` isn't already docked
            anywhere (this container's tree, or any other DockContainer such
            as a floating window) - it always adds a fresh column, never
            reuses an existing group. MainComponent::showView() uses this the
            first time a view that starts outside the default layout (see
            DockLayout::defaultLayout) is asked for. */
        DockTabGroup& addAsNewColumn (DockPanel panel);

        /** Fired when a tab is dragged out past every DockContainer's bounds
            and released - MainComponent (Task 9) uses this to spin up a
            FloatingDockWindow at the drop point. Carries the panel that was
            removed from its original tab group (ownership of `content` is
            NOT transferred - it's still owned wherever it always was, this
            just says "put this panel's tab/content somewhere new"). Fired
            from dragOperationEnded (Task 10) when a drag ends without ever
            reaching itemDropped on any DragAndDropTarget on the desktop -
            i.e. the user released the tab somewhere with no dock target
            under it at all. */
        std::function<void (DockPanel)> onPanelDraggedOutside;

        void resized() override { root->setBounds (getLocalBounds()); }

        // DragAndDropTarget
        bool isInterestedInDragSource (const SourceDetails& details) override
        {
            return isDockTabDrag (details);
        }

        void itemDragMove (const SourceDetails& details) override
        {
            const auto* group = findTabGroupAt (details.localPosition);
            lastHoveredZone = group != nullptr
                                  ? classifyDropZone (group->getBounds(),
                                                       group->getLocalPoint (this, details.localPosition))
                                  : DropZone::none;

            // No hover-highlight painting yet - drawing one needs a colour/shape
            // design this task's brief never specified. lastHoveredZone is kept
            // up to date here (and repaint() called) so that a later pass can
            // add a paintOverChildren() that reads it without touching this
            // drag-tracking logic at all.
            repaint();
        }

        void itemDragExit (const SourceDetails&) override
        {
            lastHoveredZone = DropZone::none;
            repaint();
        }

        void itemDropped (const SourceDetails& details) override;

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

        DockTabGroup* findTabGroupAt (juce::Point<int> pointInThisComponent) const;

        /** Walks the tree looking for `target` by pointer identity. When
            found, the slot that held it is folded together with `newGroup`
            into a brand-new DockSplit (`newGroup` first or second per
            `newGroupFirst`), and that split takes `target`'s old place in
            the tree - all in one step. This has to happen in one step
            rather than "extract target, then separately find-and-replace
            its old slot" because once target's own unique_ptr has been
            moved out of a DockSplit's firstChild/secondChild, there is no
            valid placeholder to leave behind in the meantime: DockSplit's
            constructor unconditionally does addAndMakeVisible(*firstChild)
            and addAndMakeVisible(*secondChild), so a transiently-null child
            there crashes immediately rather than sitting inertly until a
            second pass fills it back in. Returns true if target was found
            (and thus folded in) anywhere under `current`. */
        bool splitAroundTarget (std::unique_ptr<DockNode>& current, DockNode* target,
                                std::unique_ptr<DockTabGroup>& newGroup,
                                DockSplit::Direction direction, bool newGroupFirst);

        /** The exact mirror image of splitAroundTarget: walks the tree looking
            for the DockSplit whose first or second child IS `emptied` (by
            pointer identity), and replaces that split with its OTHER,
            surviving child - rebuilding every ancestor split on the way back
            up for the same reason splitAroundTarget does (DockSplit's
            constructor unconditionally addAndMakeVisible()s both children, so
            a transiently-null child crashes on the spot rather than waiting
            for a second pass). `emptied` is destroyed along with the split
            that held it. Returns false, changing nothing, when `emptied` has
            no parent split at all - i.e. it IS the whole tree; an empty root
            group is left standing as-is (see collapseIfEmpty). */
        bool collapseEmptyGroup (std::unique_ptr<DockNode>& current, DockTabGroup* emptied);

        /** Collapses `group` out of the tree if - and only if - a panel move
            just left it with zero panels. A no-op otherwise, so every panel-
            removal site can call it unconditionally. */
        void collapseIfEmpty (DockTabGroup* group);

        /** True only for a genuine dock-tab drag. DockContainer is a
            DragAndDropContainer, which means findParentDragContainerFor()
            resolves to it for ANY drag started by ANY component anywhere
            inside the docked views - including features that have nothing to
            do with docking (GenerateView's "drag a generated candidate onto
            the timeline" is the live example: it starts a drag described as
            "ss.candidate", whose (int) conversion is 0 - an entirely
            plausible-looking panel index). The source component's type is
            what actually tells the two apart: a real dock-tab drag is always
            started by a DockTabGroup::Tab, which is a juce::TabBarButton.

            NOTE this accepts any juce::TabBarButton, not specifically a
            DockTabGroup::Tab - safe today because no other TabBarButton
            anywhere in the app calls startDragging() (e.g. TranscribeView's
            own stemTabs is a plain, non-draggable TabbedButtonBar), but if a
            future tab bar living inside the dock tree ever grows drag-to-
            reorder behaviour of its own, it would satisfy this check too and
            reopen the exact class of bug this method exists to prevent. */
        static bool isDockTabDrag (const SourceDetails& details) noexcept
        {
            return dynamic_cast<juce::TabBarButton*> (details.sourceComponent.get()) != nullptr;
        }
    };
}
