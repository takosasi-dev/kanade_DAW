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

        // Piano Roll and the rest (transcribe/generate/notation/session/
        // modular) share one tabbed group on the right - built by listing
        // every remaining id directly in the "panels" array, since
        // defaultLayout has no live DockTabGroup to call addPanel on yet
        // (it only ever produces a var). Piano Roll used to get its own
        // dedicated column; folding it in here means a fresh install opens
        // with it reachable by tab, not shown until clicked, matching how
        // Transcribe/Generate/etc already behave by default.
        juce::Array<juce::var> restIds;
        for (const auto& kv : displayNamesById)
            if (kv.first != "timeline" && kv.first != "mixer")
                restIds.add (kv.first);

        auto* restObj = new juce::DynamicObject();
        restObj->setProperty ("type", "tabGroup");
        restObj->setProperty ("panels", restIds);
        restObj->setProperty ("active", 0);

        // Timeline | Mixer | (everything else, tabbed) as three side-by-side
        // columns, via two nested horizontal splits - roughly 33% / 33% / 33%
        // of the width.
        return makeSplit (
            makeSingleGroup ("timeline"),
            makeSplit (
                makeSingleGroup ("mixer"),
                juce::var (restObj),
                0.5),
            0.33);
    }
}
