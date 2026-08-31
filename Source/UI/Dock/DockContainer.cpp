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

    bool DockContainer::splitAroundTarget (std::unique_ptr<DockNode>& current, DockNode* target,
                                           std::unique_ptr<DockTabGroup>& newGroup,
                                           DockSplit::Direction direction, bool newGroupFirst)
    {
        if (current.get() == target)
        {
            // current and newGroup both go straight into the new DockSplit's
            // constructor here, in the same statement that overwrites
            // `current` - current's underlying object (the matched target)
            // is moved into the DockSplit's child list before `current`
            // itself is ever reassigned, so it's never without an owner.
            current = newGroupFirst
                          ? std::make_unique<DockSplit> (direction, std::move (newGroup), std::move (current))
                          : std::make_unique<DockSplit> (direction, std::move (current), std::move (newGroup));
            return true;
        }

        if (auto* split = dynamic_cast<DockSplit*> (current.get()))
        {
            auto [first, second] = split->releaseChildren();

            if (splitAroundTarget (first, target, newGroup, direction, newGroupFirst))
            {
                current = std::make_unique<DockSplit> (split->getDirection(), std::move (first),
                                                       std::move (second), split->getRatio());
                return true;
            }

            if (splitAroundTarget (second, target, newGroup, direction, newGroupFirst))
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

    bool DockContainer::collapseEmptyGroup (std::unique_ptr<DockNode>& current, DockTabGroup* emptied)
    {
        auto* split = dynamic_cast<DockSplit*> (current.get());

        if (split == nullptr)
            return false;

        auto [first, second] = split->releaseChildren();

        // A direct hit: this split's whole job was dividing `emptied` from its
        // sibling, and there's nothing left to divide - the sibling takes over
        // the split's slot outright. `current`'s reassignment destroys the old
        // split, and the emptied group goes with it when the local unique_ptr
        // still holding it falls out of scope.
        if (first.get() == static_cast<DockNode*> (emptied))
        {
            current = std::move (second);
            return true;
        }

        if (second.get() == static_cast<DockNode*> (emptied))
        {
            current = std::move (first);
            return true;
        }

        // Not this split - recurse, then put it back together either way. The
        // rebuild is unconditional (exactly as in splitAroundTarget) because
        // releaseChildren has already nulled both of the old split's children,
        // so there is no "leave it untouched" path to take.
        const auto found = collapseEmptyGroup (first, emptied)
                            || collapseEmptyGroup (second, emptied);

        current = std::make_unique<DockSplit> (split->getDirection(), std::move (first),
                                               std::move (second), split->getRatio());
        return found;
    }

    void DockContainer::collapseIfEmpty (DockTabGroup* group)
    {
        if (group == nullptr || group->getNumPanels() > 0)
            return;

        // A false return means the emptied group has no parent split - it's
        // the entire tree. Leaving a blank root group standing is the accepted
        // degenerate case: DockLayout drops empty groups on save/restore, so it
        // self-heals on the next relaunch.
        if (collapseEmptyGroup (root, group))
        {
            addAndMakeVisible (*root);
            resized();
        }
    }

    void DockContainer::itemDropped (const SourceDetails& details)
    {
        currentDragWasHandled = true;
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
                                            targetGroup->getLocalPoint (this, details.localPosition));

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

                // Moving the source group's last panel away leaves it as an
                // empty husk still taking up half a split - fold it out and let
                // its sibling have the space. A centre-join otherwise touches
                // no tree topology at all, hence the addAndMakeVisible/resized
                // living inside collapseIfEmpty rather than out here.
                collapseIfEmpty (sourceGroup);
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
        const bool newGroupFirst = (zone == DropZone::left || zone == DropZone::top);

        // Finds targetGroup by pointer identity anywhere under root and, right
        // at that spot, folds it together with newGroup into a brand-new
        // DockSplit - see splitAroundTarget's doc comment for why this has to
        // happen in a single pass rather than extract-then-graft.
        splitAroundTarget (root, static_cast<DockNode*> (targetGroup), newGroup, direction, newGroupFirst);

        // Collapsing the source's now-empty old slot runs AFTER splitAroundTarget,
        // not before: splitAroundTarget has already found and grafted around
        // targetGroup by this point, so collapsing sourceGroup here can never
        // destroy a node splitAroundTarget still needed to find - even when
        // source and target are the same group (a self-drag), where the fresh
        // split splitAroundTarget just created around sourceGroup/newGroup
        // immediately collapses back down to just newGroup, since sourceGroup
        // is the empty side.
        collapseIfEmpty (sourceGroup);

        // splitAroundTarget (and possibly collapseIfEmpty, right above) may have
        // replaced `root` with a brand-new DockSplit object (any actual split,
        // and even the "neither side matched" fallback inside splitAroundTarget,
        // always constructs a fresh DockSplit rather than reusing the old one) -
        // that new object isn't a child of `this` yet, only internally
        // self-consistent (each node parents its own children in its own
        // constructor). addChildComponent's real implementation only acts when
        // child.parentComponent != this, so this is a harmless no-op on the
        // rare path where root truly didn't change identity, and required
        // whenever it did.
        addAndMakeVisible (*root);

        resized();
        repaint();
    }

    void DockContainer::dragOperationEnded (const SourceDetails& details)
    {
        // isDockTabDrag is load-bearing, not belt-and-braces: an unrelated
        // feature's drag that started inside our subtree (GenerateView's
        // candidate cards) reaches here with currentDragWasHandled == false,
        // because whoever DID claim the drop (TimelineView) is a different
        // DragAndDropTarget entirely and never calls our itemDropped. Every
        // other guard below passes for it - (int) juce::var ("ss.candidate")
        // is 0, a valid-looking index - so without this check we'd rip out
        // panel 0 of whatever group that view happens to live in.
        if (currentDragWasHandled || onPanelDraggedOutside == nullptr || ! isDockTabDrag (details))
            return;

        auto* sourceGroup = details.sourceComponent != nullptr
                                ? details.sourceComponent->findParentComponentOfClass<DockTabGroup>()
                                : nullptr;
        const int draggedIndex = (int) details.description;

        if (sourceGroup == nullptr || draggedIndex < 0)
            return;

        auto panel = sourceGroup->removePanel (draggedIndex);
        collapseIfEmpty (sourceGroup);
        addAndMakeVisible (*root);
        resized();

        if (panel.content != nullptr)
            onPanelDraggedOutside (panel);
    }

    std::vector<DockPanel> DockContainer::extractAllPanels()
    {
        std::vector<DockPanel> extracted;

        std::function<void (DockNode&)> walk = [&] (DockNode& node)
        {
            if (auto* group = dynamic_cast<DockTabGroup*> (&node))
            {
                while (group->getNumPanels() > 0)
                    extracted.push_back (group->removePanel (0));
            }
            else if (auto* split = dynamic_cast<DockSplit*> (&node))
            {
                walk (split->getFirst());
                walk (split->getSecond());
            }
        };

        walk (*root);
        return extracted;
    }

    DockTabGroup& DockContainer::addAsNewColumn (DockPanel panel)
    {
        auto newGroup = std::make_unique<DockTabGroup>();
        newGroup->addPanel (panel);
        auto* groupPtr = newGroup.get();

        // root.get() as both the tree to search AND the target: the very
        // first identity check inside splitAroundTarget matches immediately,
        // so this always wraps the WHOLE current tree in a new split with
        // newGroup as its second child - never digs into a sub-branch.
        splitAroundTarget (root, root.get(), newGroup, DockSplit::Direction::horizontal, false);

        addAndMakeVisible (*root);
        resized();
        repaint();

        return *groupPtr;
    }
}
