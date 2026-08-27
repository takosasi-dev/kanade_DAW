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
