#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss
{
    /** One dockable screen's worth of content. `id` is one of the 8 fixed
        view identifiers ("timeline", "mixer", "pianoRoll", "transcribe",
        "generate", "notation", "session", "modular" - see Task 9) and is
        what DockLayout persists; `content` points at the already-constructed,
        already-owned-elsewhere view component (MainComponent keeps owning
        the std::unique_ptr<TimelineView> etc. exactly as it does today - a
        DockPanel never owns its content). */
    struct DockPanel
    {
        juce::String id;
        juce::String displayName;
        juce::Component* content = nullptr;
    };
}
