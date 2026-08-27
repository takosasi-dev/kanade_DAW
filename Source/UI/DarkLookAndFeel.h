#pragma once
#include <juce_gui_extra/juce_gui_extra.h>

namespace ss
{
    /** Every colour the ScoreSmith views paint with (spec 9: dark-first, high
        information density).  Components read ss::palette() instead of naming
        colours inline, so switching theme is one assignment rather than a hunt
        through every paint() method. */
    struct Palette
    {
        juce::Colour windowBg, panelBg, panelAltBg, headerBg, outline, divider;
        juce::Colour text, textDim, textBright;
        juce::Colour accent, accentDim, warning, danger, success;
        juce::Colour laneBg, laneAltBg, gridSub, gridBeat, gridBar;
        juce::Colour waveform, waveformDim, clipBg, clipSelected;
        juce::Colour note, noteSelected, noteGhost;
        juce::Colour playhead, loopRegion, recordArm;
        juce::Colour meterLow, meterMid, meterHigh;
        bool isDark = true;
    };

    /** The palette for the theme currently applied by DarkLookAndFeel. */
    const Palette& palette() noexcept;

    /** Confidence 0..1 -> note fill colour (spec 8.2-5).

        Spec 9.7 requires the confidence display to work for colour-blind users,
        so the ramp moves monotonically in LIGHTNESS and SATURATION, not just in
        hue: a deuteranope still reads "dim + muddy = uncertain, bright = sure".
        Anything under the review threshold additionally gets hatched by
        paintConfidenceHatch(), which is a redundant, purely non-colour cue. */
    juce::Colour confidenceColour (float confidence) noexcept;

    /** Diagonal hatching whose density rises as confidence falls.  Draws nothing
        at all above `threshold` so confident notes stay clean. */
    void paintConfidenceHatch (juce::Graphics&, juce::Rectangle<float> area,
                               float confidence, float threshold = 0.7f);

    /** Meter colour for a normalised 0..1 level, lightness-ordered so the
        "too hot" end reads as different even without hue. */
    juce::Colour meterColour (float normalisedLevel) noexcept;

    /** The application look and feel.  Handles all three themes from
        Settings::getTheme(): "dark", "light" and "system" (which follows the OS
        and re-applies itself when the OS setting changes). */
    class DarkLookAndFeel : public juce::LookAndFeel_V4,
                            private juce::DarkModeSettingListener
    {
    public:
        DarkLookAndFeel();
        ~DarkLookAndFeel() override;

        /** themeId is "dark" | "light" | "system" - exactly the values stored by
            Settings::setTheme(). Anything else is treated as "dark". */
        void setTheme (const juce::String& themeId);
        juce::String getTheme() const { return theme; }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        juce::Font getLabelFont (juce::Label&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        juce::Font getPopupMenuFont() override;

        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override;
        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                               juce::Slider&) override;
        void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;

    private:
        void darkModeSettingChanged() override;
        void applyTheme();

        juce::String theme { "dark" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DarkLookAndFeel)
    };
}
