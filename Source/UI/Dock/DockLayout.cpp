#include "UI/Dock/DockLayout.h"
#include "UI/Dock/DockSplit.h"
#include "UI/Dock/DockTabGroup.h"

namespace ss::DockLayout
{
    std::unique_ptr<DockNode> restore (const juce::var& state,
                                       const std::map<juce::String, juce::Component*>& panelsById,
                                       const std::map<juce::String, juce::String>& displayNamesById)
    {
        if (! state.isObject())
            return nullptr;

        const auto type = state.getProperty ("type", {}).toString();

        if (type == "split")
        {
            const auto directionStr = state.getProperty ("direction", {}).toString();
            const auto direction = directionStr == "horizontal" ? DockSplit::Direction::horizontal
                                                                 : DockSplit::Direction::vertical;
            const auto ratio = (double) state.getProperty ("ratio", 0.5);

            auto first = restore (state.getProperty ("first", {}), panelsById, displayNamesById);
            auto second = restore (state.getProperty ("second", {}), panelsById, displayNamesById);

            if (first == nullptr && second == nullptr)
                return nullptr;

            if (first == nullptr) return second;
            if (second == nullptr) return first;

            return std::make_unique<DockSplit> (direction, std::move (first), std::move (second), ratio);
        }

        if (type == "tabGroup")
        {
            auto group = std::make_unique<DockTabGroup>();
            const auto* panelIds = state.getProperty ("panels", {}).getArray();

            if (panelIds != nullptr)
            {
                for (const auto& idVar : *panelIds)
                {
                    const auto id = idVar.toString();
                    const auto it = panelsById.find (id);

                    if (it != panelsById.end())
                    {
                        const auto nameIt = displayNamesById.find (id);
                        const auto displayName = nameIt != displayNamesById.end() ? nameIt->second : id;
                        group->addPanel ({ id, displayName, it->second });
                    }
                }
            }

            const auto activeIndex = (int) state.getProperty ("active", 0);

            if (group->getNumPanels() == 0)
                return nullptr; // every panel this group named is unavailable - drop the group entirely

            group->setActivePanel (juce::jlimit (0, group->getNumPanels() - 1, activeIndex));
            return group;
        }

        return nullptr;
    }

    juce::var defaultLayout (const std::map<juce::String, juce::String>& displayNamesById)
    {
        auto makeSingleGroup = [&] (const juce::String& id) -> juce::var
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "tabGroup");
            juce::Array<juce::var> ids;
            if (displayNamesById.count (id) != 0)
                ids.add (id);
            obj->setProperty ("panels", ids);
            obj->setProperty ("active", 0);
            return juce::var (obj);
        };

        auto makeSplit = [] (const juce::var& first, const juce::var& second, double ratio)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", "split");
            obj->setProperty ("direction", "horizontal");
            obj->setProperty ("ratio", ratio);
            obj->setProperty ("first", first);
            obj->setProperty ("second", second);
            return juce::var (obj);
        };

        // Timeline | Mixer, 50/50. Piano Roll/Transcribe/Generate/Notation/
        // Session/Modular used to share a third tabbed column here, but its
        // default-active tab was whichever id happened to sort first in
        // displayNamesById (a std::map) - Generate, greeting a fresh install
        // with the busiest panel in the app for a reason that had nothing to
        // do with what most sessions actually need first. They now start
        // with no column at all: MainComponent::showView() grafts each one
        // in on its own, the first time the View menu/shortcut asks for it
        // (see DockContainer::addAsNewColumn), so nothing is lost - they are
        // simply absent until requested rather than occupying screen space
        // by default.
        return makeSplit (
            makeSingleGroup ("timeline"),
            makeSingleGroup ("mixer"),
            0.5);
    }
}
