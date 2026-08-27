#pragma once
#include "Core/AppContext.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace ss
{
    /** The whole window contents (spec 9.1): menu/toolbar on top, browser on the
        left, switchable workspace in the centre, AI panel on the right,
        transport at the bottom. */
    class MainComponent : public juce::Component,
                          public juce::ApplicationCommandTarget,
                          public juce::MenuBarModel
    {
    public:
        explicit MainComponent (AppContext&);
        ~MainComponent() override;

        enum class View { timeline, pianoRoll, mixer, transcribe, generate, notation, session, modular };
        void showView (View);
        View getCurrentView() const noexcept;

        void resized() override;
        void paint (juce::Graphics&) override;

        // MenuBarModel
        juce::StringArray getMenuBarNames() override;
        juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
        void menuItemSelected (int, int) override;

        // ApplicationCommandTarget
        /** JUCE spells this type InvocationInfo.  The alias keeps the frozen
            perform() signature below compiling; it adds nothing a caller sees. */
        using InvokedCommandInfo = juce::ApplicationCommandTarget::InvocationInfo;

        juce::ApplicationCommandTarget* getNextCommandTarget() override;
        void getAllCommands (juce::Array<juce::CommandID>&) override;
        void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
        bool perform (const InvokedCommandInfo&) override;

        juce::ApplicationCommandManager& getCommandManager() noexcept;

        /** Returns false if the user cancelled a "save changes?" prompt. */
        void tryToQuit (std::function<void (bool)> callback);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
    };
}
