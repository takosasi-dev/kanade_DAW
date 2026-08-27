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

        // A point is in a band if its fraction < 1.0. In a corner (multiple bands),
        // pick the band where the point is deepest (highest fraction < 1.0).
        float deepest = -1.0f;
        DropZone result = DropZone::centre;

        if (leftFrac < 1.0f && leftFrac > deepest) {
            deepest = leftFrac;
            result = DropZone::left;
        }
        if (rightFrac < 1.0f && rightFrac > deepest) {
            deepest = rightFrac;
            result = DropZone::right;
        }
        if (topFrac < 1.0f && topFrac > deepest) {
            deepest = topFrac;
            result = DropZone::top;
        }
        if (bottomFrac < 1.0f && bottomFrac > deepest) {
            deepest = bottomFrac;
            result = DropZone::bottom;
        }

        return result;
    }
}
