#include "UI/Dock/DockTabGroup.h"

namespace ss
{
    DockTabGroup::DockTabGroup()
    {
        addAndMakeVisible (bar);

        // Load-bearing beyond just user tab clicks: TabbedButtonBar::removeTab
        // re-fires this synchronously when the active tab shifts during a removal,
        // which is what keeps activeIndex correct there - don't assume this only
        // matters for interactive clicks.
        bar.onCurrentTabChanged = [this] (int index, const juce::String&) { setActivePanel (index); };
    }

    void DockTabGroup::addPanel (DockPanel panel, int insertIndex)
    {
        const auto index = insertIndex < 0 || insertIndex > (int) panels.size()
                                ? (int) panels.size()
                                : insertIndex;

        if (panel.content != nullptr)
            addChildComponent (panel.content);

        panels.insert (panels.begin() + index, panel);
        bar.addTab (panel.displayName, juce::Colours::transparentBlack, index);

        // TabbedButtonBar::addTab re-derives ITS OWN current-tab index by
        // tracking TabInfo* identity across the insert - correct, but done via
        // a direct field write, not through the notifying setCurrentTabIndex(),
        // so our onCurrentTabChanged callback doesn't fire on this path and
        // activeIndex would otherwise go stale when inserting before it.
        // bar.getCurrentTabIndex() is the single source of truth here.
        activeIndex = bar.getCurrentTabIndex();
        showActiveContent();
    }

    DockPanel DockTabGroup::removePanel (int index)
    {
        if (index < 0 || index >= (int) panels.size())
            return {};

        auto removed = panels[(size_t) index];

        if (removed.content != nullptr)
            removeChildComponent (removed.content);

        panels.erase (panels.begin() + index);
        bar.removeTab (index);

        if (panels.empty())
        {
            activeIndex = -1;
        }
        else
        {
            activeIndex = juce::jlimit (0, (int) panels.size() - 1, activeIndex);
            bar.setCurrentTabIndex (activeIndex, false);
            showActiveContent();
        }

        return removed;
    }

    void DockTabGroup::setActivePanel (int index)
    {
        if (index < 0 || index >= (int) panels.size())
            return;

        activeIndex = index;
        bar.setCurrentTabIndex (index, false);
        showActiveContent();
    }

    void DockTabGroup::showActiveContent()
    {
        for (int i = 0; i < (int) panels.size(); ++i)
            if (auto* c = panels[(size_t) i].content)
                c->setVisible (i == activeIndex);

        resized();
    }

    const DockPanel* DockTabGroup::getPanel (int index) const noexcept
    {
        if (index < 0 || index >= (int) panels.size())
            return nullptr;

        return &panels[(size_t) index];
    }

    int DockTabGroup::indexOfPanel (const juce::String& id) const noexcept
    {
        for (int i = 0; i < (int) panels.size(); ++i)
            if (panels[(size_t) i].id == id)
                return i;

        return -1;
    }

    void DockTabGroup::resized()
    {
        auto area = getLocalBounds();
        bar.setBounds (area.removeFromTop (24));

        if (activeIndex >= 0 && activeIndex < (int) panels.size())
            if (auto* c = panels[(size_t) activeIndex].content)
                c->setBounds (area);
    }

    juce::var DockTabGroup::toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("type", "tabGroup");

        juce::Array<juce::var> panelIds;
        for (const auto& p : panels)
            panelIds.add (p.id);

        obj->setProperty ("panels", panelIds);
        obj->setProperty ("active", activeIndex);
        return juce::var (obj);
    }
}
