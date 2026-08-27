#pragma once
#include "UI/Dock/DockNode.h"
#include <map>

namespace ss::DockLayout
{
    /** Rebuilds a DockNode tree from a var produced by DockNode::toVar()
        (or DockContainer::toVar()). `panelsById` supplies the live
        juce::Component* for each of the 8 fixed view ids (Task 9 builds
        this map); a saved reference to an id not present in the map is
        dropped silently (that view isn't available in this build/session -
        fail soft, not by refusing the whole layout). `displayNamesById`
        supplies the human-readable label for each id (the same shape
        defaultLayout() takes); an id absent from this map falls back to
        using the raw id string as its own display name rather than
        failing - this only degrades a label, it should never drop a
        panel that panelsById says IS available. If dropping ids empties
        out one side of a DockSplit, the split collapses to just its
        surviving side rather than being rebuilt with a null child. Returns
        nullptr if `state` isn't a well-formed tree at all (wrong "type" tag,
        missing required fields, etc.) - the caller falls back to
        defaultLayout(). */
    std::unique_ptr<DockNode> restore (const juce::var& state,
                                       const std::map<juce::String, juce::Component*>& panelsById,
                                       const std::map<juce::String, juce::String>& displayNamesById);

    /** Timeline, Mixer, and Piano Roll each get their own side-by-side
        column (roughly 33% / 33% / 17% / 17% of the width via nested
        splits), with the remaining views tabbed together in the last
        column - the day-one default before any layout has ever been saved.
        Any id in `displayNamesById` not one of the three named columns
        falls into that tabbed group; any of the three column ids missing
        from `displayNamesById` just produces an empty column (which
        restore() then collapses away) rather than crashing. */
    juce::var defaultLayout (const std::map<juce::String, juce::String>& displayNamesById);

    /** The reserved name Settings::getDockLayout/setDockLayout use for
        "whatever the layout looked like when the app last closed" - never
        shown to the user as a pickable named layout. */
    inline const juce::String lastSessionLayoutName = "__last__";
}
