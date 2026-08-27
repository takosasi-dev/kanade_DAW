#include "UI/DarkLookAndFeel.h"

namespace ss
{
    // The live palette.  Message-thread only, so a plain file-static is enough -
    // there is exactly one LookAndFeel and exactly one UI thread.
    static Palette current;

    const Palette& palette() noexcept { return current; }

    static Palette makeDarkPalette()
    {
        Palette p;
        p.isDark      = true;
        p.windowBg    = juce::Colour (0xff14161b);
        p.panelBg     = juce::Colour (0xff1c1f26);
        p.panelAltBg  = juce::Colour (0xff23272f);
        p.headerBg    = juce::Colour (0xff2a2f38);
        p.outline     = juce::Colour (0xff3a4049);
        p.divider     = juce::Colour (0xff111317);

        p.text        = juce::Colour (0xffd6dae1);
        p.textDim     = juce::Colour (0xff858d99);
        p.textBright  = juce::Colour (0xfff2f5f9);

        p.accent      = juce::Colour (0xff4aa3df);
        p.accentDim   = juce::Colour (0xff2d6182);
        p.warning     = juce::Colour (0xffe0a33a);
        p.danger      = juce::Colour (0xffe05c4a);
        p.success     = juce::Colour (0xff5fb87a);

        p.laneBg      = juce::Colour (0xff191c22);
        p.laneAltBg   = juce::Colour (0xff1e222a);
        p.gridSub     = juce::Colour (0xff23272f);
        p.gridBeat    = juce::Colour (0xff2c313a);
        p.gridBar     = juce::Colour (0xff414855);

        p.waveform    = juce::Colour (0xff7fc4ee);
        p.waveformDim = juce::Colour (0xff3e6d8c);
        p.clipBg      = juce::Colour (0xff2b3947);
        p.clipSelected= juce::Colour (0xff3d5a72);

        p.note        = juce::Colour (0xff6cc2f0);
        p.noteSelected= juce::Colour (0xffffd479);
        p.noteGhost   = juce::Colour (0x40a0a8b4);

        p.playhead    = juce::Colour (0xfff5f7fa);
        p.loopRegion  = juce::Colour (0x2263b7e8);
        p.recordArm   = juce::Colour (0xffe0453a);

        p.meterLow    = juce::Colour (0xff3f9e63);
        p.meterMid    = juce::Colour (0xffd9b93c);
        p.meterHigh   = juce::Colour (0xffe8503f);
        return p;
    }

    static Palette makeLightPalette()
    {
        Palette p;
        p.isDark      = false;
        p.windowBg    = juce::Colour (0xffeceef2);
        p.panelBg     = juce::Colour (0xfff5f6f9);
        p.panelAltBg  = juce::Colour (0xffe6e9ef);
        p.headerBg    = juce::Colour (0xffdde1e9);
        p.outline     = juce::Colour (0xffbcc2cd);
        p.divider     = juce::Colour (0xffc7ccd6);

        p.text        = juce::Colour (0xff23272f);
        p.textDim     = juce::Colour (0xff6a7280);
        p.textBright  = juce::Colour (0xff0d1117);

        p.accent      = juce::Colour (0xff1c6ea4);
        p.accentDim   = juce::Colour (0xff8dbdda);
        p.warning     = juce::Colour (0xff9a6b12);
        p.danger      = juce::Colour (0xffb03426);
        p.success     = juce::Colour (0xff2f7a48);

        p.laneBg      = juce::Colour (0xfff8f9fb);
        p.laneAltBg   = juce::Colour (0xffeef0f5);
        p.gridSub     = juce::Colour (0xffe1e4ea);
        p.gridBeat    = juce::Colour (0xffd2d7e0);
        p.gridBar     = juce::Colour (0xffa9b1bf);

        p.waveform    = juce::Colour (0xff2b6d94);
        p.waveformDim = juce::Colour (0xff89b4cd);
        p.clipBg      = juce::Colour (0xffcfdde9);
        p.clipSelected= juce::Colour (0xffa8c8de);

        p.note        = juce::Colour (0xff1f6f9c);
        p.noteSelected= juce::Colour (0xffb5761b);
        p.noteGhost   = juce::Colour (0x3a4a515c);

        p.playhead    = juce::Colour (0xff14161b);
        p.loopRegion  = juce::Colour (0x221c6ea4);
        p.recordArm   = juce::Colour (0xffc03429);

        p.meterLow    = juce::Colour (0xff2f7a48);
        p.meterMid    = juce::Colour (0xff9a7712);
        p.meterHigh   = juce::Colour (0xffb03426);
        return p;
    }

    //==============================================================================
    juce::Colour confidenceColour (float confidence) noexcept
    {
        const auto c = juce::jlimit (0.0f, 1.0f, confidence);

        // Hue moves as well, but only as a secondary cue: brightness and
        // saturation carry the information on their own (spec 9.7).
        const float hue = juce::jmap (c, 0.075f, 0.52f);

        if (current.isDark)
            return juce::Colour::fromHSV (hue, juce::jmap (c, 0.88f, 0.52f),
                                          juce::jmap (c, 0.40f, 0.96f), 1.0f);

        // On a light lane the same ordering has to run the other way: washed-out
        // means uncertain, deep and solid means certain.
        return juce::Colour::fromHSV (hue, juce::jmap (c, 0.42f, 0.80f),
                                      juce::jmap (c, 0.92f, 0.48f), 1.0f);
    }

    void paintConfidenceHatch (juce::Graphics& g, juce::Rectangle<float> area,
                               float confidence, float threshold)
    {
        if (confidence >= threshold || area.getWidth() < 2.0f || area.getHeight() < 2.0f)
            return;

        // Denser stripes the less certain we are - a cue that survives greyscale
        // printing and every form of colour blindness.
        const float t       = juce::jlimit (0.0f, 1.0f, confidence / juce::jmax (0.0001f, threshold));
        const float spacing = juce::jmap (t, 3.0f, 9.0f);

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area.getSmallestIntegerContainer());
        g.setColour ((current.isDark ? juce::Colours::black : juce::Colours::white).withAlpha (0.45f));

        for (float x = area.getX() - area.getHeight(); x < area.getRight(); x += spacing)
            g.drawLine (x, area.getBottom(), x + area.getHeight(), area.getY(), 1.0f);
    }

    juce::Colour meterColour (float normalisedLevel) noexcept
    {
        const auto v = juce::jlimit (0.0f, 1.0f, normalisedLevel);
        if (v > 0.92f) return current.meterHigh;
        if (v > 0.75f) return current.meterMid;
        return current.meterLow;
    }

    //==============================================================================
    DarkLookAndFeel::DarkLookAndFeel()
    {
        juce::Desktop::getInstance().addDarkModeSettingListener (this);
        applyTheme();
    }

    DarkLookAndFeel::~DarkLookAndFeel()
    {
        juce::Desktop::getInstance().removeDarkModeSettingListener (this);
    }

    void DarkLookAndFeel::setTheme (const juce::String& themeId)
    {
        theme = themeId;
        applyTheme();
    }

    void DarkLookAndFeel::darkModeSettingChanged()
    {
        if (theme == "system")
            applyTheme();
    }

    void DarkLookAndFeel::applyTheme()
    {
        const bool wantDark = theme == "light" ? false
                            : theme == "system" ? juce::Desktop::getInstance().isDarkModeActive()
                                                : true;

        current = wantDark ? makeDarkPalette() : makeLightPalette();
        const auto& p = current;

        auto scheme = wantDark ? getDarkColourScheme() : getLightColourScheme();
        using UIColour = juce::LookAndFeel_V4::ColourScheme::UIColour;
        scheme.setUIColour (UIColour::windowBackground, p.windowBg);
        scheme.setUIColour (UIColour::widgetBackground, p.panelBg);
        scheme.setUIColour (UIColour::menuBackground,   p.panelAltBg);
        scheme.setUIColour (UIColour::outline,          p.outline);
        scheme.setUIColour (UIColour::defaultText,      p.text);
        scheme.setUIColour (UIColour::defaultFill,      p.accent);
        scheme.setUIColour (UIColour::highlightedText,  p.textBright);
        scheme.setUIColour (UIColour::highlightedFill,  p.accentDim);
        scheme.setUIColour (UIColour::menuText,         p.text);
        setColourScheme (scheme);

        setColour (juce::ResizableWindow::backgroundColourId, p.windowBg);
        setColour (juce::Label::textColourId,                 p.text);
        setColour (juce::TextButton::buttonColourId,          p.panelAltBg);
        setColour (juce::TextButton::buttonOnColourId,        p.accentDim);
        setColour (juce::TextButton::textColourOffId,         p.text);
        setColour (juce::TextButton::textColourOnId,          p.textBright);
        setColour (juce::ComboBox::backgroundColourId,        p.panelAltBg);
        setColour (juce::ComboBox::textColourId,              p.text);
        setColour (juce::ComboBox::outlineColourId,           p.outline);
        setColour (juce::ComboBox::arrowColourId,             p.textDim);
        setColour (juce::TextEditor::backgroundColourId,      p.laneBg);
        setColour (juce::TextEditor::textColourId,            p.text);
        setColour (juce::TextEditor::outlineColourId,         p.outline);
        setColour (juce::TextEditor::highlightColourId,       p.accentDim);
        setColour (juce::Slider::thumbColourId,               p.accent);
        setColour (juce::Slider::trackColourId,               p.accentDim);
        setColour (juce::Slider::backgroundColourId,          p.laneBg);
        setColour (juce::Slider::rotarySliderFillColourId,    p.accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, p.panelAltBg);
        setColour (juce::Slider::textBoxTextColourId,         p.text);
        setColour (juce::Slider::textBoxOutlineColourId,      p.outline.withAlpha (0.4f));
        setColour (juce::ScrollBar::thumbColourId,            p.outline);
        setColour (juce::ScrollBar::trackColourId,            p.laneBg);
        setColour (juce::ListBox::backgroundColourId,         p.panelBg);
        setColour (juce::ListBox::textColourId,               p.text);
        setColour (juce::TreeView::backgroundColourId,        p.panelBg);
        setColour (juce::TreeView::linesColourId,             p.outline);
        setColour (juce::TabbedComponent::backgroundColourId, p.panelBg);
        setColour (juce::TabbedComponent::outlineColourId,    p.outline);
        setColour (juce::TabbedButtonBar::tabOutlineColourId, p.outline);
        setColour (juce::TabbedButtonBar::frontTextColourId,  p.textBright);
        setColour (juce::TabbedButtonBar::tabTextColourId,    p.textDim);
        setColour (juce::PopupMenu::backgroundColourId,       p.panelAltBg);
        setColour (juce::PopupMenu::textColourId,             p.text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, p.accentDim);
        setColour (juce::PopupMenu::highlightedTextColourId,  p.textBright);
        setColour (juce::ProgressBar::backgroundColourId,     p.laneBg);
        setColour (juce::ProgressBar::foregroundColourId,     p.accent);
        setColour (juce::ToggleButton::textColourId,          p.text);
        setColour (juce::ToggleButton::tickColourId,          p.accent);
        setColour (juce::ToggleButton::tickDisabledColourId,  p.outline);
        setColour (juce::AlertWindow::backgroundColourId,     p.panelBg);
        setColour (juce::AlertWindow::textColourId,           p.text);
        setColour (juce::AlertWindow::outlineColourId,        p.outline);
        setColour (juce::DocumentWindow::textColourId,        p.text);
        setColour (juce::TableHeaderComponent::backgroundColourId, p.headerBg);
        setColour (juce::TableHeaderComponent::textColourId,  p.textDim);
        setColour (juce::TableHeaderComponent::outlineColourId, p.outline);
        setColour (juce::TooltipWindow::backgroundColourId,   p.headerBg);
        setColour (juce::TooltipWindow::textColourId,         p.text);
        setColour (juce::TooltipWindow::outlineColourId,      p.outline);

        // Repaint everything that is already on screen.
        for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;)
            if (auto* c = juce::Desktop::getInstance().getComponent (i))
                c->repaint();
    }

    // Heights are clamped, not just min'd: a component that has not been laid out
    // yet reports zero, and a zero-height font is a JUCE assertion.
    juce::Font DarkLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
    {
        return juce::Font (juce::FontOptions ((float) juce::jlimit (10, 15, buttonHeight - 8)));
    }

    juce::Font DarkLookAndFeel::getLabelFont (juce::Label& label)
    {
        return juce::Font (juce::FontOptions ((float) juce::jlimit (10, 15, label.getHeight() - 4)));
    }

    juce::Font DarkLookAndFeel::getComboBoxFont (juce::ComboBox& box)
    {
        return juce::Font (juce::FontOptions ((float) juce::jlimit (10, 14, box.getHeight() - 6)));
    }

    juce::Font DarkLookAndFeel::getPopupMenuFont()
    {
        return juce::Font (juce::FontOptions (15.0f));
    }

    void DarkLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                                const juce::Colour& backgroundColour,
                                                bool highlighted, bool down)
    {
        const auto& p = current;
        auto area = b.getLocalBounds().toFloat().reduced (0.5f);

        auto fill = backgroundColour;
        if (down)             fill = fill.brighter (p.isDark ? 0.28f : 0.12f);
        else if (highlighted) fill = fill.brighter (p.isDark ? 0.14f : 0.06f);

        g.setColour (fill);
        g.fillRoundedRectangle (area, 3.0f);
        g.setColour (p.outline.withAlpha (b.isEnabled() ? 1.0f : 0.4f));
        g.drawRoundedRectangle (area, 3.0f, 1.0f);
    }

    void DarkLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float rotaryStartAngle,
                                            float rotaryEndAngle, juce::Slider& s)
    {
        const auto& p = current;
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const auto radius    = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre    = bounds.getCentre();
        const auto thickness = juce::jmax (3.0f, radius * 0.24f);
        const auto arcRadius = radius - thickness * 0.5f;
        const auto angle     = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        juce::Path back;
        back.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (s.findColour (juce::Slider::rotarySliderOutlineColourId));
        g.strokePath (back, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        if (s.isEnabled())
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 rotaryStartAngle, angle, true);
            g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
            g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }

        juce::Point<float> tip (centre.x + arcRadius * std::sin (angle),
                                centre.y - arcRadius * std::cos (angle));
        g.setColour (p.textBright);
        g.drawLine (juce::Line<float> (centre, tip).withShortenedStart (arcRadius * 0.35f),
                    juce::jmax (1.5f, thickness * 0.4f));
    }

    void DarkLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float minSliderPos, float maxSliderPos,
                                            juce::Slider::SliderStyle style, juce::Slider& s)
    {
        const auto& p = current;

        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                              minSliderPos, maxSliderPos, style, s);
            return;
        }

        const bool vertical = style == juce::Slider::LinearVertical;
        auto area = juce::Rectangle<int> (x, y, width, height).toFloat();

        // A DAW fader: a thin track with a wide, grippy cap.
        auto track = vertical ? area.withSizeKeepingCentre (5.0f, area.getHeight())
                              : area.withSizeKeepingCentre (area.getWidth(), 5.0f);
        g.setColour (p.laneBg);
        g.fillRoundedRectangle (track, 2.5f);
        g.setColour (p.outline);
        g.drawRoundedRectangle (track, 2.5f, 1.0f);

        auto filled = track;
        if (vertical) filled = filled.withTop (sliderPos);
        else          filled = filled.withRight (sliderPos);
        g.setColour (s.findColour (juce::Slider::trackColourId));
        g.fillRoundedRectangle (filled, 2.5f);

        auto cap = vertical ? juce::Rectangle<float> (area.getWidth() * 0.85f, 14.0f)
                                  .withCentre ({ area.getCentreX(), sliderPos })
                            : juce::Rectangle<float> (11.0f, area.getHeight() * 0.85f)
                                  .withCentre ({ sliderPos, area.getCentreY() });
        g.setColour (s.isEnabled() ? p.headerBg.brighter (p.isDark ? 0.25f : 0.0f) : p.panelAltBg);
        g.fillRoundedRectangle (cap, 2.0f);
        g.setColour (s.findColour (juce::Slider::thumbColourId));
        g.drawRoundedRectangle (cap.reduced (0.5f), 2.0f, 1.2f);
        g.drawLine (vertical ? cap.getX() + 2.0f : cap.getCentreX(),
                    vertical ? cap.getCentreY() : cap.getY() + 2.0f,
                    vertical ? cap.getRight() - 2.0f : cap.getCentreX(),
                    vertical ? cap.getCentreY() : cap.getBottom() - 2.0f, 1.0f);
    }
}
