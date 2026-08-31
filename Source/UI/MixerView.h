#pragma once
#include "Mixer/BuiltinFx.h"
#include "UI/UiSupport.h"

namespace ss
{
    class ChannelStrip;
    class MixerView;

    /** Generic editor for one built-in effect, generated from
        BuiltinEffect::getParameterInfo() (spec 8.4.6).  There is deliberately no
        per-effect UI: the effects are internal and fixed, so a table of sliders
        built from their own parameter list is the whole job. */
    class BuiltinFxEditor final : public juce::Component
    {
    public:
        /** busId != 0 edits that bus's chain; otherwise trackId == invalidTrackId
            edits the master chain and anything else edits a track's. */
        BuiltinFxEditor (AppContext&, TrackId, int slotIndex, int busId = 0);

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        BuiltinFxSlot* slot() const;
        void pushToEffect();
        void commit();

        AppContext& ctx;
        TrackId     trackId;
        int         index;
        int         busId;

        juce::Label       titleLabel;
        juce::ToggleButton bypassButton;
        juce::OwnedArray<juce::Slider> sliders;
        juce::OwnedArray<juce::Label>  labels;
        std::vector<BuiltinEffect::ParamInfo> info;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltinFxEditor)
    };

    //==============================================================================
    /** One channel strip.  `busId != 0` is a bus (spec 8.4.5); otherwise
        trackId == invalidTrackId is the master strip, which additionally carries
        the LUFS / peak readout (spec 8.4.6). */
    class MixerStrip final : public juce::Component
    {
    public:
        MixerStrip (AppContext&, UiState&, MixerView&, TrackId, int busId = 0);

        TrackId getTrackId() const noexcept { return trackId; }
        int     getBusId()   const noexcept { return busId; }
        bool    isBus()      const noexcept { return busId != 0; }
        bool    isMaster()   const noexcept { return trackId == invalidTrackId && busId == 0; }

        void refresh();
        void updateMeters();

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        Track* track() const;
        Bus*   bus() const;
        /** Index of this strip's bus in Project::buses, or -1. */
        int    busIndex() const;
        /** The live strip this one drives - a track strip, a bus strip or none. */
        ChannelStrip* liveStrip() const;

        void   commit (const juce::String&, std::function<void (Track&)>);
        void   commitBus (const juce::String&, std::function<void (Bus&)>);
        /** One undoable edit of whichever built-in chain this strip shows. */
        void   commitChain (const juce::String&, std::function<void (std::vector<BuiltinFxSlot>&)>);

        void   rebuildSlotButtons();
        void   showAddFxMenu();
        void   showSlotMenu (int buttonIndex);
        void   showOutputMenu();

        AppContext& ctx;
        UiState&    ui;
        MixerView&  owner;
        TrackId     trackId;
        int         busId;

        juce::Label      nameLabel, valueLabel, loudnessLabel;
        juce::Slider     fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
        juce::Slider     panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
        juce::Label      panValueLabel;   // "L20"/"C"/"R45" - shown only when Settings::getShowPanValueLabel()
        juce::TextButton muteButton, soloButton, addFxButton, outButton;
        juce::OwnedArray<juce::TextButton> slotButtons;

        /** Which chain entry each slot button refers to. */
        struct SlotRef { bool builtin; int index; };
        std::vector<SlotRef> slotRefs;

        juce::Rectangle<float> meterArea;
        float peak[2] { 0.0f, 0.0f }, rms[2] { 0.0f, 0.0f };
        bool  updating = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrip)
    };

    //==============================================================================
    /** The mixer (spec 8.4.5 / 8.4.6): one strip per track plus a master strip. */
    class MixerView final : public ProjectView,
                            private juce::Timer
    {
    public:
        MixerView (AppContext&, UiState&);
        ~MixerView() override;

        /** Opens (or re-shows) a plugin's own editor in its own window. */
        void showPluginEditor (TrackId, int pluginIndex);
        /** Opens the generic editor for a built-in effect slot. */
        void showBuiltinFxEditor (TrackId, int slotIndex, int busId = 0);

        /** Preferences > General "Mixer meter redraw rate" - juce::Timer is a
            private base, so this is the settings-change hook's only way in. */
        void setRefreshHz (int hz) { startTimerHz (hz); }

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

    private:
        class PluginWindow;

        void timerCallback() override;
        void rebuildStrips();

        // stripHolder is declared first so it outlives the viewport that points at it.
        juce::Component  stripHolder;
        juce::Viewport   viewport;
        juce::OwnedArray<MixerStrip> strips;      // tracks first, then buses
        std::unique_ptr<MixerStrip>  masterStrip;
        juce::TextButton addBusButton;
        juce::OwnedArray<PluginWindow> pluginWindows;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
    };
}
