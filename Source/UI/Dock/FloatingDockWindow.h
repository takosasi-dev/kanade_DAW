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
        explicit FloatingDockWindow (std::unique_ptr<DockNode> rootContent);
        ~FloatingDockWindow() override = default;

        void closeButtonPressed() override;

        DockContainer& getDockContainer() const noexcept { return *container; }

        /** Fired from closeButtonPressed when the user closes this window.
            The handler is what actually destroys the window: MainComponent
            (Task 9) erases the owning unique_ptr from its `floatingWindows`
            vector here, so `this` is dead by the time this callback returns
            and closeButtonPressed touches nothing afterwards. The handler
            also rescues the window's panels back into the main workspace
            first (via DockContainer::extractAllPanels) - otherwise they'd be
            orphaned for the rest of the session. */
        std::function<void (FloatingDockWindow*)> onClosing;

    private:
        std::unique_ptr<DockContainer> container;
    };
}
