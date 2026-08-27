#include "UI/Dock/FloatingDockWindow.h"
#include <limits>

namespace ss
{
    namespace FloatingDockWindowGeometry
    {
        juce::Rectangle<int> clampToNearestDisplay (juce::Rectangle<int> savedBounds,
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

    FloatingDockWindow::FloatingDockWindow (std::unique_ptr<DockNode> rootContent)
        : juce::DocumentWindow ("KANADE DAW", juce::Colours::darkgrey,
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

    void FloatingDockWindow::closeButtonPressed()
    {
        // No `delete this` here, deliberately: this window is owned by a
        // std::unique_ptr in MainComponent's `floatingWindows` vector, and
        // onClosing's handler erases that entry - which destroys `this`
        // synchronously, right here, inside the call below. Deleting again
        // afterwards would run on already-freed memory. Nothing is touched
        // after onClosing returns for exactly that reason.
        if (onClosing)
            onClosing (this);
    }
}
