#include "UI/ExtensionSettingsDialog.h"

namespace ss
{
    ExtensionSettingsDialog::ExtensionSettingsDialog (const std::vector<ExtensionSetting>& settings,
                                                       std::function<void (Result)> onComplete_)
        : onComplete (std::move (onComplete_))
    {
        for (const auto& setting : settings)
        {
            auto row = std::make_unique<Row>();
            row->envVar = setting.envVar;
            row->type = setting.type;

            row->label = std::make_unique<juce::Label>();
            row->label->setText (setting.label, juce::dontSendNotification);
            row->label->setFont (juce::Font (juce::FontOptions (13.0f)));
            row->label->setColour (juce::Label::textColourId, palette().text);
            row->label->setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (*row->label);

            switch (setting.type)
            {
                case ExtensionSettingType::slider:
                {
                    auto slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                                   juce::Slider::TextBoxRight);
                    slider->setRange (setting.sliderMin, setting.sliderMax);
                    slider->setValue (setting.sliderDefault, juce::dontSendNotification);
                    slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
                    addAndMakeVisible (*slider);
                    row->control = std::move (slider);
                    break;
                }
                case ExtensionSettingType::checkbox:
                {
                    auto toggle = std::make_unique<juce::ToggleButton>();
                    toggle->setToggleState (setting.checkboxDefault, juce::dontSendNotification);
                    addAndMakeVisible (*toggle);
                    row->control = std::move (toggle);
                    break;
                }
                case ExtensionSettingType::dropdown:
                {
                    auto combo = std::make_unique<juce::ComboBox>();
                    for (int i = 0; i < setting.dropdownOptions.size(); ++i)
                        combo->addItem (setting.dropdownOptions[i], i + 1);
                    const int defaultIndex = setting.dropdownOptions.indexOf (setting.dropdownDefault);
                    combo->setSelectedId (defaultIndex >= 0 ? defaultIndex + 1 : 1, juce::dontSendNotification);
                    addAndMakeVisible (*combo);
                    row->control = std::move (combo);
                    break;
                }
            }

            rows.add (std::move (row));
        }

        okButton.setButtonText (TRANS ("OK"));
        okButton.onClick = [this]
        {
            std::map<juce::String, juce::String> values;
            for (const auto* row : rows)
            {
                switch (row->type)
                {
                    case ExtensionSettingType::slider:
                        values[row->envVar] = juce::String (
                            static_cast<juce::Slider*> (row->control.get())->getValue());
                        break;
                    case ExtensionSettingType::checkbox:
                        values[row->envVar] = static_cast<juce::ToggleButton*> (row->control.get())
                                                  ->getToggleState() ? "1" : "0";
                        break;
                    case ExtensionSettingType::dropdown:
                        values[row->envVar] = static_cast<juce::ComboBox*> (row->control.get())->getText();
                        break;
                }
            }
            finish (std::move (values));
        };
        addAndMakeVisible (okButton);

        cancelButton.setButtonText (TRANS ("Cancel"));
        cancelButton.onClick = [this] { finish (std::nullopt); };
        addAndMakeVisible (cancelButton);

        setSize (480, 12 + (int) settings.size() * 36 + 12 + 30 + 12);
    }

    ExtensionSettingsDialog::~ExtensionSettingsDialog()
    {
        // The dialog can also close via the window's own close button or
        // Escape, neither of which goes through okButton/cancelButton -
        // treat any of those the same as Cancel.
        if (! completed && onComplete)
            onComplete (std::nullopt);
    }

    void ExtensionSettingsDialog::finish (Result result)
    {
        if (completed) return;
        completed = true;

        if (onComplete)
            onComplete (std::move (result));

        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    }

    void ExtensionSettingsDialog::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void ExtensionSettingsDialog::resized()
    {
        auto area = getLocalBounds().reduced (14, 12);

        auto buttonRow = area.removeFromBottom (30);
        okButton.setBounds (buttonRow.removeFromRight (90));
        buttonRow.removeFromRight (8);
        cancelButton.setBounds (buttonRow.removeFromRight (90));
        area.removeFromBottom (8);

        for (auto* row : rows)
        {
            auto r = area.removeFromTop (28);
            row->label->setBounds (r.removeFromLeft (220));
            row->control->setBounds (r);
            area.removeFromTop (8);
        }
    }

    void ExtensionSettingsDialog::launch (const juce::String& extensionName,
                                          const std::vector<ExtensionSetting>& settings,
                                          std::function<void (Result)> onComplete)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new ExtensionSettingsDialog (settings, std::move (onComplete)));
        options.dialogTitle            = extensionName;
        options.dialogBackgroundColour = palette().windowBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = true;
        options.launchAsync();
    }
}
