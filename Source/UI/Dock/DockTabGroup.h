#pragma once
#include "UI/Dock/DockNode.h"
#include "UI/Dock/DockPanel.h"
#include <vector>

namespace ss
{
    /** A tab strip over one or more DockPanels, showing exactly one panel's
        `content` at a time. Built on juce::TabbedButtonBar (subclassed, not
        juce::TabbedComponent - TabbedComponent hardcodes its own tabs-plus-
        content resized() layout with no override point, which fights a
        dock's need to own that layout itself; TabbedButtonBar just manages
        tab buttons/names and leaves content-swapping to the caller). */
    class DockTabGroup final : public DockNode
    {
    public:
        DockTabGroup();

        void addPanel (DockPanel panel, int insertIndex = -1);
        DockPanel removePanel (int index);
        void setActivePanel (int index);

        int getNumPanels() const noexcept                  { return (int) panels.size(); }
        int getActiveIndex() const noexcept                { return activeIndex; }
        const DockPanel* getPanel (int index) const noexcept;
        int indexOfPanel (const juce::String& id) const noexcept;

        void resized() override;
        juce::var toVar() const override;

    private:
        /** Gives individual tabs mouse-drag behaviour (reorder within this
            bar via TabbedButtonBar::moveTab, or hand off to DockContainer's
            drag-and-drop machinery once the drag leaves this bar's bounds -
            wired up in Task 5, this class only fires the drag start). */
        class Tab final : public juce::TabBarButton
        {
        public:
            Tab (const juce::String& name, juce::TabbedButtonBar& bar) : juce::TabBarButton (name, bar) {}

            void mouseDrag (const juce::MouseEvent& e) override
            {
                juce::TabBarButton::mouseDrag (e);

                if (e.mouseWasDraggedSinceMouseDown() && ! dragStarted)
                {
                    if (auto* dd = juce::DragAndDropContainer::findParentDragContainerFor (this))
                    {
                        dragStarted = true;
                        dd->startDragging (juce::var (getIndex()), this, juce::ScaledImage(), true);
                    }
                }
            }

            void mouseUp (const juce::MouseEvent& e) override
            {
                juce::TabBarButton::mouseUp (e);
                dragStarted = false;
            }

        private:
            bool dragStarted = false;
        };

        class Bar final : public juce::TabbedButtonBar
        {
        public:
            Bar() : juce::TabbedButtonBar (juce::TabbedButtonBar::TabsAtTop) {}

            std::function<void (int, const juce::String&)> onCurrentTabChanged;

        protected:
            juce::TabBarButton* createTabButton (const juce::String& tabName, int tabIndex) override
            {
                juce::ignoreUnused (tabIndex);
                return new Tab (tabName, *this);
            }

            void currentTabChanged (int newIndex, const juce::String& newName) override
            {
                if (onCurrentTabChanged)
                    onCurrentTabChanged (newIndex, newName);
            }
        };

        Bar bar;
        std::vector<DockPanel> panels;
        int activeIndex = -1;

        void showActiveContent();
    };
}
