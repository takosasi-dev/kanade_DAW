#include "UI/Dock/DockSplit.h"

namespace ss
{
    DockSplit::DockSplit (Direction direction, std::unique_ptr<DockNode> first,
                          std::unique_ptr<DockNode> second, double ratio)
        : dir (direction),
          initialRatio (ratio),
          firstChild (std::move (first)),
          secondChild (std::move (second)),
          resizerBar (&layoutManager, 1, direction == Direction::horizontal)
    {
        addAndMakeVisible (*firstChild);
        addAndMakeVisible (*secondChild);
        addAndMakeVisible (resizerBar);

        // Item 0 = firstChild, item 1 = the resizer bar, item 2 = secondChild.
        // Both panels can shrink to 40px before the resizer refuses to move
        // further; a negative preferredSize is StretchableLayoutManager's
        // convention for "a proportion of the available space" (see
        // juce_StretchableLayoutManager.h's setItemLayout docs and
        // MainComponent.cpp:401-407's identical use of -1.0/-ratio-style
        // values for its own browser/workspace/AI-panel split).
        layoutManager.setItemLayout (0, 40.0, -1.0, -ratio);
        layoutManager.setItemLayout (1, resizerBarThickness, resizerBarThickness, resizerBarThickness);
        layoutManager.setItemLayout (2, 40.0, -1.0, -(1.0 - ratio));
    }

    double DockSplit::getRatio() const noexcept
    {
        // getItemCurrentAbsoluteSize is the per-item pixel size (only
        // getItemCurrentPosition(0) is misleading here - by definition it's
        // always 0, since "position" means "sum of every earlier item's
        // size", and there's nothing before item 0). Both sizes are 0 until
        // resized() has actually laid out real pixels, which never happens
        // for a DockSplit that hasn't been given a parent/bounds yet - fall
        // back to the ratio this split was constructed with in that case.
        const auto firstSize = layoutManager.getItemCurrentAbsoluteSize (0);
        const auto secondSize = layoutManager.getItemCurrentAbsoluteSize (2);
        const auto total = firstSize + secondSize;
        return total > 0 ? (double) firstSize / (double) total : initialRatio;
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

        // Direction::horizontal = panels side by side, divided by an upright
        // bar dragged left/right (isBarVertical=true above, matching
        // MainComponent.cpp:225-226's leftBar/rightBar) - which is
        // layOutComponents' vertically=false case ("placed side-by-side in a
        // horizontal line"). Direction::vertical stacks them instead, which
        // is vertically=true.
        layoutManager.layOutComponents (comps, 3, 0, 0, getWidth(), getHeight(),
                                        dir == Direction::vertical, true);
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
