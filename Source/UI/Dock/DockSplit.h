#pragma once
#include "UI/Dock/DockNode.h"
#include <juce_gui_basics/juce_gui_basics.h>

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

        /** Releases ownership of both children (for DockContainer to move one
            of them elsewhere and discard the split itself, e.g. when a group
            empties out and its sibling should take over the whole space). */
        std::pair<std::unique_ptr<DockNode>, std::unique_ptr<DockNode>> releaseChildren();

        void resized() override;
        juce::var toVar() const override;

    private:
        Direction dir;

        // StretchableLayoutManager only exposes current pixel sizes, not a
        // ratio, and those are all 0 until this component has actually been
        // given real bounds and laid out at least once (e.g. right after
        // construction, before any parent has sized it). getRatio() falls
        // back to this when there's no live pixel data to derive from yet.
        double initialRatio;

        std::unique_ptr<DockNode> firstChild, secondChild;
        juce::StretchableLayoutManager layoutManager;
        juce::StretchableLayoutResizerBar resizerBar;

        static constexpr int resizerBarThickness = 6;
    };
}
