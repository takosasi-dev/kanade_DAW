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
