#include "UI/PreferencesDialog.h"
#include "Core/Localisation.h"
#include "Core/Settings.h"
#include "Engine/AudioEngine.h"
#include "Extensions/FormatExtensionManager.h"
#include "Plugins/PluginManager.h"
#include <algorithm>
#include <cmath>
#include <juce_audio_utils/juce_audio_utils.h>   // AudioDeviceSelectorComponent

namespace ss
{
    namespace
    {
        constexpr int rowHeight = 26;
        constexpr int labelWidth = 190;

        const double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0,
                                       176400.0, 192000.0, 352800.0, 384000.0 };
        const int    bitDepths[]   = { 16, 24, 32 };

        /** A left-labelled settings row, which is the entire layout language of
            this dialog. */
        struct Row
        {
            static juce::Rectangle<int> next (juce::Rectangle<int>& area, int h = rowHeight)
            {
                auto r = area.removeFromTop (h);
                area.removeFromTop (6);
                return r;
            }
        };

        void layOutRow (juce::Rectangle<int>& area, juce::Label& label, juce::Component& control,
                        int h = rowHeight)
        {
            auto row = Row::next (area, h);
            label.setBounds (row.removeFromLeft (labelWidth));
            control.setBounds (row);
        }

        void setUpLabel (juce::Component& parent, juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.0f)));
            l.setColour (juce::Label::textColourId, palette().text);
            parent.addAndMakeVisible (l);
        }

        void setUpNote (juce::Component& parent, juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (11.5f)));
            l.setColour (juce::Label::textColourId, palette().textDim);
            l.setJustificationType (juce::Justification::topLeft);
            parent.addAndMakeVisible (l);
        }

        //======================================================================
        // 1. Audio
        //======================================================================
        class AudioTab final : public juce::Component,
                               private juce::Timer
        {
        public:
            AudioTab (AppContext& c, std::function<void()> changed)
                : ctx (c), onChanged (std::move (changed))
            {
                if (ctx.engine != nullptr)
                {
                    // The device selector covers device, driver type, sample rate,
                    // buffer size and input channel routing - re-implementing that
                    // list would only get out of step with the driver.
                    selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                                   ctx.engine->getDeviceManager(), 0, 64, 0, 64, false, false, true, false);
                    viewport.setViewedComponent (selector.get(), false);
                    viewport.setScrollBarsShown (true, false);
                    addAndMakeVisible (viewport);
                }

                setUpLabel (*this, bitDepthLabel, TRANS ("Recording bit depth"));
                bitDepthBox.addItem ("16-bit", 1);
                bitDepthBox.addItem ("24-bit", 2);
                bitDepthBox.addItem ("32-bit float", 3);
                bitDepthBox.onChange = [this]
                {
                    if (ctx.project != nullptr)
                        ctx.project->bitDepth = bitDepths[juce::jlimit (0, 2, bitDepthBox.getSelectedId() - 1)];
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (bitDepthBox);

                if (ctx.project != nullptr)
                    for (int i = 0; i < 3; ++i)
                        if (bitDepths[i] == ctx.project->bitDepth)
                            bitDepthBox.setSelectedId (i + 1, juce::dontSendNotification);

                setUpNote (*this, latencyLabel, {});
                setUpNote (*this, routingNote,
                           TRANS ("Input routing: pick each track's hardware input on its track header. "
                                  "ASIO is recommended on Windows; WASAPI exclusive mode is used when the "
                                  "ASIO SDK was not available at build time."));

                startTimer (500);
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);
                auto bottom = area.removeFromBottom (120);
                viewport.setBounds (area);

                if (selector != nullptr)
                    selector->setSize (juce::jmax (360, area.getWidth() - 16), 340);

                layOutRow (bottom, bitDepthLabel, bitDepthBox);
                latencyLabel.setBounds (Row::next (bottom, 20));
                routingNote.setBounds (bottom);
            }

        private:
            void timerCallback() override
            {
                if (ctx.engine == nullptr)
                    return;

                const double ms = ctx.engine->getLatencyMs();
                latencyLabel.setText (TRANS ("Predicted round-trip latency") + ": "
                                        + juce::String (ms, 1) + " ms",
                                      juce::dontSendNotification);
            }

            AppContext& ctx;
            std::function<void()> onChanged;
            juce::Viewport viewport;
            std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
            juce::Label bitDepthLabel, latencyLabel, routingNote;
            juce::ComboBox bitDepthBox;
        };

        //======================================================================
        // 2. MIDI
        //======================================================================
        class MidiTab final : public juce::Component,
                              private juce::ListBoxModel
        {
        public:
            explicit MidiTab (AppContext& c) : ctx (c)
            {
                devices = juce::MidiInput::getAvailableDevices();

                setUpLabel (*this, inputsLabel, TRANS ("MIDI inputs"));
                list.setModel (this);
                list.setRowHeight (24);
                addAndMakeVisible (list);

                setUpLabel (*this, outputLabel, TRANS ("MIDI output"));
                outputBox.addItem (TRANS ("None"), 1);
                {
                    const auto outputs = juce::MidiOutput::getAvailableDevices();
                    for (int i = 0; i < outputs.size(); ++i)
                        outputBox.addItem (outputs[i].name, i + 2);
                }
                outputBox.setSelectedId (1, juce::dontSendNotification);
                outputBox.onChange = [this]
                {
                    if (ctx.engine == nullptr) return;
                    const auto outputs = juce::MidiOutput::getAvailableDevices();
                    const int index = outputBox.getSelectedId() - 2;
                    ctx.engine->getDeviceManager().setDefaultMidiOutputDevice (
                        index >= 0 && index < outputs.size() ? outputs[index].identifier : juce::String());
                };
                addAndMakeVisible (outputBox);

                thruButton.setButtonText (TRANS ("MIDI thru (echo input to the armed track's instrument)"));
                if (ctx.settings != nullptr)
                    thruButton.setToggleState (ctx.settings->raw().getBoolValue ("midi.thru", true),
                                               juce::dontSendNotification);
                thruButton.onClick = [this]
                {
                    if (ctx.settings != nullptr)
                        ctx.settings->raw().setValue ("midi.thru", thruButton.getToggleState());
                };
                addAndMakeVisible (thruButton);

                setUpNote (*this, learnNote,
                           TRANS ("MIDI Learn: right-click any knob or slider and move a control on your "
                                  "hardware to bind it. Bindings are stored with your preferences."));
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);
                inputsLabel.setBounds (Row::next (area, 20));
                list.setBounds (area.removeFromTop (juce::jmax (80, area.getHeight() - 140)));
                area.removeFromTop (10);
                layOutRow (area, outputLabel, outputBox);
                thruButton.setBounds (Row::next (area, 24));
                learnNote.setBounds (area);
            }

        private:
            int getNumRows() override { return devices.size(); }

            void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
            {
                if (row < 0 || row >= devices.size())
                    return;

                const bool enabled = ctx.engine != nullptr
                                       && ctx.engine->getDeviceManager().isMidiInputDeviceEnabled (
                                              devices[row].identifier);

                g.setColour (enabled ? palette().accent : palette().outline);
                g.drawRect (6, height / 2 - 7, 14, 14, 1);
                if (enabled)
                    g.fillRect (9, height / 2 - 4, 8, 8);

                g.setColour (palette().text);
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (devices[row].name, 28, 0, width - 34, height, juce::Justification::centredLeft, true);
            }

            void listBoxItemClicked (int row, const juce::MouseEvent&) override
            {
                if (ctx.engine == nullptr || row < 0 || row >= devices.size())
                    return;

                auto& dm = ctx.engine->getDeviceManager();
                const auto id = devices[row].identifier;
                dm.setMidiInputDeviceEnabled (id, ! dm.isMidiInputDeviceEnabled (id));
                list.repaintRow (row);
            }

            AppContext& ctx;
            juce::Array<juce::MidiDeviceInfo> devices;
            juce::ListBox list;
            juce::Label inputsLabel, outputLabel, learnNote;
            juce::ComboBox outputBox;
            juce::ToggleButton thruButton;
        };

        //======================================================================
        // 3. General / Appearance
        //======================================================================
        class GeneralTab final : public juce::Component
        {
        public:
            GeneralTab (AppContext& c, DarkLookAndFeel& laf, std::function<void()> changed)
                : ctx (c), lookAndFeel (laf), onChanged (std::move (changed))
            {
                auto& settings = *ctx.settings;

                setUpLabel (*this, languageLabel, TRANS ("Language"));
                languageBox.addItem ("English", 1);
                languageBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e")), 2);
                languageBox.setSelectedId (settings.getLanguage() == "ja" ? 2 : 1, juce::dontSendNotification);
                languageBox.onChange = [this]
                {
                    const juce::String code = languageBox.getSelectedId() == 2 ? "ja" : "en";
                    ctx.settings->setLanguage (code);
                    setUiLanguage (code);
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (languageBox);

                setUpLabel (*this, themeLabel, TRANS ("Theme"));
                themeBox.addItem (TRANS ("Dark"), 1);
                themeBox.addItem (TRANS ("Light"), 2);
                themeBox.addItem (TRANS ("Follow system"), 3);
                themeBox.setSelectedId (settings.getTheme() == "light" ? 2
                                          : settings.getTheme() == "system" ? 3 : 1,
                                        juce::dontSendNotification);
                themeBox.onChange = [this]
                {
                    const juce::String theme = themeBox.getSelectedId() == 2 ? "light"
                                             : themeBox.getSelectedId() == 3 ? "system" : "dark";
                    ctx.settings->setTheme (theme);
                    lookAndFeel.setTheme (theme);
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (themeBox);

                setUpLabel (*this, scaleLabel, TRANS ("UI scale"));
                scaleSlider.setRange (0.75, 2.0, 0.05);
                scaleSlider.setValue (settings.getUiScale(), juce::dontSendNotification);
                scaleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
                scaleSlider.onDragEnd = [this]
                {
                    ctx.settings->setUiScale ((float) scaleSlider.getValue());
                    juce::Desktop::getInstance().setGlobalScaleFactor ((float) scaleSlider.getValue());
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (scaleSlider);

                setUpLabel (*this, startupLabel, TRANS ("On startup"));
                startupBox.addItem (TRANS ("New empty project"), 1);
                startupBox.addItem (TRANS ("Reopen the last project"), 2);
                startupBox.addItem (TRANS ("Show the project picker"), 3);
                startupBox.setSelectedId (settings.getStartupBehaviour() == "last" ? 2
                                            : settings.getStartupBehaviour() == "picker" ? 3 : 1,
                                          juce::dontSendNotification);
                startupBox.onChange = [this]
                {
                    ctx.settings->setStartupBehaviour (startupBox.getSelectedId() == 2 ? "last"
                                                        : startupBox.getSelectedId() == 3 ? "picker" : "new");
                };
                addAndMakeVisible (startupBox);

                setUpLabel (*this, autoSaveLabel, TRANS ("Autosave every"));
                autoSaveSlider.setRange (0.0, 1800.0, 30.0);
                autoSaveSlider.setValue (settings.getAutoSaveIntervalSeconds(), juce::dontSendNotification);
                autoSaveSlider.setTextValueSuffix (" " + TRANS ("seconds"));
                autoSaveSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 110, 22);
                autoSaveSlider.onDragEnd = [this]
                {
                    ctx.settings->setAutoSaveIntervalSeconds ((int) autoSaveSlider.getValue());
                };
                addAndMakeVisible (autoSaveSlider);

                setUpLabel (*this, backupsLabel, TRANS ("Backup generations"));
                backupsSlider.setRange (0.0, 50.0, 1.0);
                backupsSlider.setValue (settings.getBackupGenerations(), juce::dontSendNotification);
                backupsSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
                backupsSlider.onDragEnd = [this]
                {
                    ctx.settings->setBackupGenerations ((int) backupsSlider.getValue());
                };
                addAndMakeVisible (backupsSlider);

                panValueLabelButton.setButtonText (TRANS ("Show pan value as a number next to the pan knob"));
                panValueLabelButton.setToggleState (settings.getShowPanValueLabel(), juce::dontSendNotification);
                panValueLabelButton.onClick = [this]
                {
                    ctx.settings->setShowPanValueLabel (panValueLabelButton.getToggleState());
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (panValueLabelButton);

                setUpLabel (*this, timelineHzLabel, TRANS ("Timeline redraw rate"));
                timelineHzSlider.setRange (5.0, 60.0, 1.0);
                timelineHzSlider.setValue (settings.getTimelineRefreshHz(), juce::dontSendNotification);
                timelineHzSlider.setTextValueSuffix (" Hz");
                timelineHzSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
                timelineHzSlider.onDragEnd = [this]
                {
                    ctx.settings->setTimelineRefreshHz ((int) timelineHzSlider.getValue());
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (timelineHzSlider);

                setUpLabel (*this, mixerHzLabel, TRANS ("Mixer meter redraw rate"));
                mixerHzSlider.setRange (5.0, 60.0, 1.0);
                mixerHzSlider.setValue (settings.getMixerMeterRefreshHz(), juce::dontSendNotification);
                mixerHzSlider.setTextValueSuffix (" Hz");
                mixerHzSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
                mixerHzSlider.onDragEnd = [this]
                {
                    ctx.settings->setMixerMeterRefreshHz ((int) mixerHzSlider.getValue());
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (mixerHzSlider);

                setUpLabel (*this, undoLimitLabel, TRANS ("Undo history limit"));
                undoLimitSlider.setRange (10.0, 5000.0, 10.0);
                undoLimitSlider.setValue (settings.getUndoHistoryLimit(), juce::dontSendNotification);
                undoLimitSlider.setTextValueSuffix (" " + TRANS ("steps"));
                undoLimitSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 22);
                undoLimitSlider.onDragEnd = [this]
                {
                    ctx.settings->setUndoHistoryLimit ((int) undoLimitSlider.getValue());
                    if (onChanged) onChanged();
                };
                addAndMakeVisible (undoLimitSlider);

                setUpNote (*this, languageNote,
                           TRANS ("Theme and scale apply immediately. A language change applies to menus and "
                                  "dialogs straight away; restart to re-translate every open panel."));
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);
                layOutRow (area, languageLabel, languageBox);
                layOutRow (area, themeLabel, themeBox);
                layOutRow (area, scaleLabel, scaleSlider);
                layOutRow (area, startupLabel, startupBox);
                layOutRow (area, autoSaveLabel, autoSaveSlider);
                layOutRow (area, backupsLabel, backupsSlider);
                panValueLabelButton.setBounds (Row::next (area, 24));
                layOutRow (area, timelineHzLabel, timelineHzSlider);
                layOutRow (area, mixerHzLabel, mixerHzSlider);
                layOutRow (area, undoLimitLabel, undoLimitSlider);
                area.removeFromTop (10);
                languageNote.setBounds (area.removeFromTop (60));
            }

        private:
            AppContext& ctx;
            DarkLookAndFeel& lookAndFeel;
            std::function<void()> onChanged;
            juce::Label languageLabel, themeLabel, scaleLabel, startupLabel, autoSaveLabel,
                        backupsLabel, timelineHzLabel, mixerHzLabel, undoLimitLabel, languageNote;
            juce::ComboBox languageBox, themeBox, startupBox;
            juce::Slider scaleSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::Slider autoSaveSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::Slider backupsSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::Slider timelineHzSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::Slider mixerHzSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::Slider undoLimitSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::ToggleButton panValueLabelButton;
        };

        //======================================================================
        // 4. Project defaults
        //======================================================================
        class ProjectDefaultsTab final : public juce::Component
        {
        public:
            explicit ProjectDefaultsTab (AppContext& c) : ctx (c)
            {
                auto& settings = *ctx.settings;

                setUpLabel (*this, bpmLabel, TRANS ("Default tempo"));
                bpmSlider.setRange (20.0, 300.0, 0.5);
                bpmSlider.setValue (settings.getDefaultBpm(), juce::dontSendNotification);
                bpmSlider.setTextValueSuffix (" BPM");
                bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 22);
                bpmSlider.onDragEnd = [this] { ctx.settings->setDefaultBpm (bpmSlider.getValue()); };
                addAndMakeVisible (bpmSlider);

                setUpLabel (*this, timeSigLabel, TRANS ("Default time signature"));
                for (int n = 1; n <= 16; ++n) timeSigNumerator.addItem (juce::String (n), n);
                for (int d : { 1, 2, 4, 8, 16 }) timeSigDenominator.addItem (juce::String (d), d);
                timeSigNumerator.setSelectedId (settings.raw().getIntValue ("project.defaultTimeSigNum", 4),
                                                juce::dontSendNotification);
                timeSigDenominator.setSelectedId (settings.raw().getIntValue ("project.defaultTimeSigDen", 4),
                                                  juce::dontSendNotification);
                timeSigNumerator.onChange = [this]
                {
                    ctx.settings->raw().setValue ("project.defaultTimeSigNum", timeSigNumerator.getSelectedId());
                };
                timeSigDenominator.onChange = [this]
                {
                    ctx.settings->raw().setValue ("project.defaultTimeSigDen", timeSigDenominator.getSelectedId());
                };
                addAndMakeVisible (timeSigNumerator);
                addAndMakeVisible (timeSigDenominator);

                setUpLabel (*this, sampleRateLabel, TRANS ("Default sample rate"));
                for (int i = 0; i < (int) juce::numElementsInArray (sampleRates); ++i)
                    sampleRateBox.addItem (juce::String (sampleRates[i] / 1000.0, 1) + " kHz", i + 1);
                for (int i = 0; i < (int) juce::numElementsInArray (sampleRates); ++i)
                    if (std::abs (sampleRates[i] - settings.getDefaultSampleRate()) < 1.0)
                        sampleRateBox.setSelectedId (i + 1, juce::dontSendNotification);
                if (sampleRateBox.getSelectedId() == 0)
                    sampleRateBox.setSelectedId (2, juce::dontSendNotification);
                sampleRateBox.onChange = [this]
                {
                    ctx.settings->setDefaultSampleRate (
                        sampleRates[juce::jlimit (0, (int) juce::numElementsInArray (sampleRates) - 1,
                                                  sampleRateBox.getSelectedId() - 1)]);
                };
                addAndMakeVisible (sampleRateBox);

                setUpLabel (*this, bitDepthLabel, TRANS ("Default bit depth"));
                bitDepthBox.addItem ("16-bit", 1);
                bitDepthBox.addItem ("24-bit", 2);
                bitDepthBox.addItem ("32-bit float", 3);
                for (int i = 0; i < 3; ++i)
                    if (bitDepths[i] == settings.getDefaultBitDepth())
                        bitDepthBox.setSelectedId (i + 1, juce::dontSendNotification);
                if (bitDepthBox.getSelectedId() == 0)
                    bitDepthBox.setSelectedId (2, juce::dontSendNotification);
                bitDepthBox.onChange = [this]
                {
                    ctx.settings->setDefaultBitDepth (bitDepths[juce::jlimit (0, 2, bitDepthBox.getSelectedId() - 1)]);
                };
                addAndMakeVisible (bitDepthBox);

                setUpLabel (*this, templateLabel, TRANS ("Default track template"));
                templateBox.addItem (TRANS ("Empty"), 1);
                templateBox.addItem (TRANS ("Singer-songwriter (vocal + guitar)"), 2);
                templateBox.addItem (TRANS ("Band (drums, bass, keys, guitar, vocal)"), 3);
                templateBox.setSelectedId (settings.raw().getIntValue ("project.defaultTemplate", 1),
                                           juce::dontSendNotification);
                templateBox.onChange = [this]
                {
                    ctx.settings->raw().setValue ("project.defaultTemplate", templateBox.getSelectedId());
                };
                addAndMakeVisible (templateBox);

                setUpNote (*this, note, TRANS ("These apply to newly created projects only."));
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);
                layOutRow (area, bpmLabel, bpmSlider);

                auto row = Row::next (area);
                timeSigLabel.setBounds (row.removeFromLeft (labelWidth));
                timeSigNumerator.setBounds (row.removeFromLeft (74));
                row.removeFromLeft (4);
                timeSigDenominator.setBounds (row.removeFromLeft (74));

                layOutRow (area, sampleRateLabel, sampleRateBox);
                layOutRow (area, bitDepthLabel, bitDepthBox);
                layOutRow (area, templateLabel, templateBox);
                area.removeFromTop (8);
                note.setBounds (area.removeFromTop (40));
            }

        private:
            AppContext& ctx;
            juce::Label bpmLabel, timeSigLabel, sampleRateLabel, bitDepthLabel, templateLabel, note;
            juce::Slider bpmSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
            juce::ComboBox timeSigNumerator, timeSigDenominator, sampleRateBox, bitDepthBox, templateBox;
        };

        //======================================================================
        // 5. Plugin management
        //======================================================================
        class PluginTab final : public juce::Component,
                                private juce::ListBoxModel,
                                private juce::Timer,
                                private juce::ChangeListener
        {
        public:
            explicit PluginTab (AppContext& c) : ctx (c)
            {
                setUpLabel (*this, pathsLabel, TRANS ("Scan paths"));
                pathsEditor.setMultiLine (true, false);
                pathsEditor.setReturnKeyStartsNewLine (true);
                pathsEditor.setText (ctx.settings->getPluginScanPaths().joinIntoString ("\n"),
                                     juce::dontSendNotification);
                pathsEditor.onFocusLost = [this]
                {
                    juce::StringArray paths;
                    paths.addLines (pathsEditor.getText());
                    paths.removeEmptyStrings();
                    ctx.settings->setPluginScanPaths (paths);
                };
                addAndMakeVisible (pathsEditor);

                addPathButton.setButtonText (TRANS ("Add folder..."));
                addPathButton.onClick = [this]
                {
                    chooser = std::make_unique<juce::FileChooser> (TRANS ("Add a plugin folder"));
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [this] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;

                        auto paths = ctx.settings->getPluginScanPaths();
                        paths.addIfNotAlreadyThere (folder.getFullPathName());
                        ctx.settings->setPluginScanPaths (paths);
                        pathsEditor.setText (paths.joinIntoString ("\n"), juce::dontSendNotification);
                    });
                };
                addAndMakeVisible (addPathButton);

                rescanButton.setButtonText (TRANS ("Rescan all"));
                rescanButton.onClick = [this]
                {
                    if (ctx.plugins != nullptr)
                    {
                        ctx.plugins->startScan (true);
                        startTimerHz (8);
                    }
                };
                addAndMakeVisible (rescanButton);

                abortButton.setButtonText (TRANS ("Stop scan"));
                abortButton.onClick = [this] { if (ctx.plugins != nullptr) ctx.plugins->abortScan(); };
                addAndMakeVisible (abortButton);

                sandboxButton.setButtonText (TRANS ("Scan and run plugins in a separate process"));
                sandboxButton.setToggleState (ctx.settings->getSandboxPlugins(), juce::dontSendNotification);
                sandboxButton.onClick = [this]
                {
                    ctx.settings->setSandboxPlugins (sandboxButton.getToggleState());
                };
                addAndMakeVisible (sandboxButton);

                autoScanButton.setButtonText (TRANS ("Scan for new plugins on startup"));
                autoScanButton.setToggleState (ctx.settings->getScanPluginsOnStartup(), juce::dontSendNotification);
                autoScanButton.onClick = [this]
                {
                    ctx.settings->setScanPluginsOnStartup (autoScanButton.getToggleState());
                };
                addAndMakeVisible (autoScanButton);

                setUpNote (*this, sandboxNote,
                           TRANS ("Sandboxing costs a little latency but means a plugin that crashes takes down "
                                  "the sandbox, not KANADE DAW. Turn it off only if you know the plugins you use "
                                  "are stable."));

                setUpLabel (*this, listLabel, TRANS ("Installed plugins - click a row to blacklist or restore it"));
                list.setModel (this);
                list.setRowHeight (24);
                addAndMakeVisible (list);

                setUpNote (*this, statusLabel, {});

                if (ctx.plugins != nullptr)
                    ctx.plugins->addChangeListener (this);

                refreshList();
            }

            ~PluginTab() override
            {
                if (ctx.plugins != nullptr)
                    ctx.plugins->removeChangeListener (this);
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);

                auto pathsRow = area.removeFromTop (84);
                pathsLabel.setBounds (pathsRow.removeFromLeft (labelWidth).removeFromTop (rowHeight));
                auto buttons = pathsRow.removeFromRight (130);
                addPathButton.setBounds (buttons.removeFromTop (26));
                buttons.removeFromTop (5);
                rescanButton.setBounds (buttons.removeFromTop (26));
                buttons.removeFromTop (5);
                abortButton.setBounds (buttons.removeFromTop (26));
                pathsEditor.setBounds (pathsRow.withTrimmedRight (8));

                area.removeFromTop (8);
                sandboxButton.setBounds (Row::next (area, 24));
                autoScanButton.setBounds (Row::next (area, 24));
                sandboxNote.setBounds (area.removeFromTop (46));
                area.removeFromTop (6);
                statusLabel.setBounds (area.removeFromTop (18));
                listLabel.setBounds (area.removeFromTop (20));
                list.setBounds (area);
            }

        private:
            void refreshList()
            {
                types.clear();
                blacklist.clear();
                if (ctx.plugins != nullptr)
                {
                    types = ctx.plugins->getKnownPluginList().getTypes();
                    blacklist = ctx.plugins->getBlacklist();
                }
                list.updateContent();
                list.repaint();
            }

            void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshList(); }

            void timerCallback() override
            {
                if (ctx.plugins == nullptr)
                    return;

                if (ctx.plugins->isScanning())
                {
                    statusLabel.setText (TRANS ("Scanning") + " "
                                           + juce::String (juce::roundToInt (ctx.plugins->getScanProgress() * 100.0f))
                                           + "%   " + ctx.plugins->getCurrentlyScannedPlugin(),
                                         juce::dontSendNotification);
                }
                else
                {
                    statusLabel.setText (juce::String (types.size()) + " " + TRANS ("plugins available"),
                                         juce::dontSendNotification);
                    stopTimer();
                    refreshList();
                }
            }

            int getNumRows() override { return types.size(); }

            void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
            {
                if (row < 0 || row >= types.size())
                    return;

                const auto id = types.getReference (row).createIdentifierString();
                const bool blacklisted = blacklist.contains (id);

                g.setColour (blacklisted ? palette().danger : palette().success);
                g.fillRect (4, height / 2 - 5, 10, 10);

                g.setColour (blacklisted ? palette().textDim : palette().text);
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (types.getReference (row).name
                              + "   (" + types.getReference (row).pluginFormatName + ", "
                              + types.getReference (row).manufacturerName + ")"
                              + (blacklisted ? "   " + TRANS ("blacklisted") : juce::String()),
                            22, 0, width - 26, height, juce::Justification::centredLeft, true);
            }

            void listBoxItemClicked (int row, const juce::MouseEvent&) override
            {
                if (ctx.plugins == nullptr || row < 0 || row >= types.size())
                    return;

                const auto id = types.getReference (row).createIdentifierString();
                const bool nowBlacklisted = ! blacklist.contains (id);
                ctx.plugins->setBlacklisted (id, nowBlacklisted);

                if (nowBlacklisted) blacklist.add (id);
                else                blacklist.removeString (id);

                list.repaintRow (row);
            }

            AppContext& ctx;
            juce::Array<juce::PluginDescription> types;
            juce::StringArray blacklist;
            juce::ListBox list;
            juce::TextEditor pathsEditor;
            juce::Label pathsLabel, listLabel, sandboxNote, statusLabel;
            juce::TextButton addPathButton, rescanButton, abortButton;
            juce::ToggleButton sandboxButton, autoScanButton;
            std::unique_ptr<juce::FileChooser> chooser;
        };

        //======================================================================
        // 6. Format extensions
        //======================================================================
        class ExtensionsTab final : public juce::Component
        {
        public:
            explicit ExtensionsTab (AppContext& c) : ctx (c)
            {
                setUpLabel (*this, pathsLabel, TRANS ("Scan paths"));
                pathsEditor.setMultiLine (true, false);
                pathsEditor.setReturnKeyStartsNewLine (true);
                pathsEditor.setText (ctx.settings->getExtensionScanPaths().joinIntoString ("\n"),
                                     juce::dontSendNotification);
                pathsEditor.onFocusLost = [this]
                {
                    juce::StringArray paths;
                    paths.addLines (pathsEditor.getText());
                    paths.removeEmptyStrings();
                    ctx.settings->setExtensionScanPaths (paths);
                    rescan();
                };
                addAndMakeVisible (pathsEditor);

                addPathButton.setButtonText (TRANS ("Add folder..."));
                addPathButton.onClick = [this]
                {
                    chooser = std::make_unique<juce::FileChooser> (TRANS ("Add an extensions folder"));
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [this] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;

                        auto paths = ctx.settings->getExtensionScanPaths();
                        paths.addIfNotAlreadyThere (folder.getFullPathName());
                        ctx.settings->setExtensionScanPaths (paths);
                        pathsEditor.setText (paths.joinIntoString ("\n"), juce::dontSendNotification);
                        rescan();
                    });
                };
                addAndMakeVisible (addPathButton);

                setUpNote (*this, helpNote,
                           TRANS ("See Help > \"How to build a format extension...\" for the manifest.json "
                                  "schema and command-line contract an extension must follow."));

                setUpLabel (*this, listLabel, TRANS ("Discovered extensions"));
                list.setMultiLine (true, true);
                list.setReadOnly (true);
                list.setCaretVisible (false);
                addAndMakeVisible (list);

                rescan();
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);

                auto pathsRow = area.removeFromTop (84);
                pathsLabel.setBounds (pathsRow.removeFromLeft (labelWidth).removeFromTop (rowHeight));
                addPathButton.setBounds (pathsRow.removeFromRight (130).removeFromTop (26));
                pathsEditor.setBounds (pathsRow.withTrimmedRight (8));

                area.removeFromTop (8);
                helpNote.setBounds (area.removeFromTop (32));
                area.removeFromTop (6);
                listLabel.setBounds (area.removeFromTop (20));
                list.setBounds (area);
            }

        private:
            void rescan()
            {
                juce::StringArray warnings;
                if (ctx.formatExtensions != nullptr)
                    ctx.formatExtensions->rescan (ctx.settings->getExtensionScanPaths(), warnings);

                juce::String text;
                if (ctx.formatExtensions != nullptr)
                    for (const auto& ext : ctx.formatExtensions->getExtensions())
                        text += ext.name + " v" + (ext.version.isEmpty() ? juce::String ("-") : ext.version)
                                + " (." + ext.fileExtension + ") - " + directionLabel (ext.direction) + "\n";

                if (! warnings.isEmpty())
                {
                    text += "\n" + TRANS ("Warnings") + ":\n";
                    for (const auto& w : warnings)
                        text += w + "\n";
                }

                list.setText (text.trimEnd(), false);
            }

            static juce::String directionLabel (ExtensionDirection d)
            {
                switch (d)
                {
                    case ExtensionDirection::importOnly: return TRANS ("Import");
                    case ExtensionDirection::exportOnly: return TRANS ("Export");
                    default:                             return TRANS ("Import") + " / " + TRANS ("Export");
                }
            }

            AppContext& ctx;
            juce::Label pathsLabel, listLabel, helpNote;
            juce::TextEditor pathsEditor, list;
            juce::TextButton addPathButton;
            std::unique_ptr<juce::FileChooser> chooser;
        };

        //======================================================================
        // 6. Keyboard shortcuts
        //======================================================================
        /** Grabs the next key combination the user presses. */
        class KeyCatcher final : public juce::Component
        {
        public:
            explicit KeyCatcher (std::function<void (juce::KeyPress)> cb)
                : callback (std::move (cb))
            {
                setWantsKeyboardFocus (true);
                setSize (300, 76);
            }

            void paint (juce::Graphics& g) override
            {
                g.fillAll (palette().panelAltBg);
                g.setColour (palette().text);
                g.setFont (juce::Font (juce::FontOptions (14.0f)));
                g.drawFittedText (TRANS ("Press the key combination to assign, or Escape to cancel"),
                                  getLocalBounds().reduced (12), juce::Justification::centred, 3);
            }

            void visibilityChanged() override
            {
                if (isVisible())
                {
                    juce::Component::SafePointer<KeyCatcher> self (this);
                    juce::Timer::callAfterDelay (60, [self] { if (self != nullptr) self->grabKeyboardFocus(); });
                }
            }

            bool keyPressed (const juce::KeyPress& key) override
            {
                if (key != juce::KeyPress::escapeKey && callback)
                    callback (key);

                if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();

                return true;
            }

        private:
            std::function<void (juce::KeyPress)> callback;
        };

        class ShortcutTab final : public juce::Component,
                                  private juce::ListBoxModel
        {
        public:
            ShortcutTab (AppContext& c, juce::ApplicationCommandManager& cm) : ctx (c), commands (cm)
            {
                search.setTextToShowWhenEmpty (TRANS ("Search commands..."), palette().textDim);
                search.onTextChange = [this] { rebuild(); };
                addAndMakeVisible (search);

                setUpLabel (*this, presetLabel, TRANS ("Keymap preset"));
                presetBox.addItem ("KANADE DAW", 1);
                presetBox.addItem ("Ableton Live", 2);
                presetBox.addItem ("Cubase", 3);
                presetBox.addItem ("Studio One", 4);
                {
                    const auto preset = ctx.settings->getKeymapPreset();
                    presetBox.setSelectedId (preset == "ableton" ? 2 : preset == "cubase" ? 3
                                               : preset == "studioone" ? 4 : 1,
                                             juce::dontSendNotification);
                }
                presetBox.onChange = [this] { applyPreset(); };
                addAndMakeVisible (presetBox);

                assignButton.setButtonText (TRANS ("Assign key..."));
                assignButton.onClick = [this] { assignToSelectedRow(); };
                addAndMakeVisible (assignButton);

                clearButton.setButtonText (TRANS ("Clear"));
                clearButton.onClick = [this]
                {
                    const int row = list.getSelectedRow();
                    if (row < 0 || row >= (int) visible.size()) return;

                    if (auto* mappings = commands.getKeyMappings())
                    {
                        for (const auto& key : mappings->getKeyPressesAssignedToCommand (visible[(size_t) row]))
                            mappings->removeKeyPress (key);
                        saveMappings();
                    }
                    list.repaintRow (row);
                };
                addAndMakeVisible (clearButton);

                resetButton.setButtonText (TRANS ("Reset all to defaults"));
                resetButton.onClick = [this]
                {
                    if (auto* mappings = commands.getKeyMappings())
                    {
                        mappings->resetToDefaultMappings();
                        saveMappings();
                    }
                    list.repaint();
                };
                addAndMakeVisible (resetButton);

                list.setModel (this);
                list.setRowHeight (24);
                addAndMakeVisible (list);

                rebuild();
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);

                auto top = Row::next (area);
                search.setBounds (top.removeFromLeft (top.getWidth() / 2 - 6));
                top.removeFromLeft (12);
                presetLabel.setBounds (top.removeFromLeft (120));
                presetBox.setBounds (top);

                auto buttons = area.removeFromBottom (28);
                assignButton.setBounds (buttons.removeFromLeft (130));
                buttons.removeFromLeft (6);
                clearButton.setBounds (buttons.removeFromLeft (90));
                buttons.removeFromLeft (6);
                resetButton.setBounds (buttons.removeFromLeft (180));

                area.removeFromBottom (8);
                list.setBounds (area);
            }

        private:
            void rebuild()
            {
                visible.clear();
                const auto filter = search.getText().trim();

                for (int i = 0; i < commands.getNumCommands(); ++i)
                {
                    const auto* info = commands.getCommandForIndex (i);
                    if (info == nullptr) continue;

                    if (filter.isEmpty()
                         || info->shortName.containsIgnoreCase (filter)
                         || info->description.containsIgnoreCase (filter)
                         || info->categoryName.containsIgnoreCase (filter))
                        visible.push_back (info->commandID);
                }

                list.updateContent();
                list.repaint();
            }

            void saveMappings()
            {
                if (auto* mappings = commands.getKeyMappings())
                {
                    auto xml = mappings->createXml (true);
                    ctx.settings->setKeyMappings (xml.get());
                }
            }

            void assignToSelectedRow()
            {
                const int row = list.getSelectedRow();
                if (row < 0 || row >= (int) visible.size())
                    return;

                const auto commandID = visible[(size_t) row];

                auto catcher = std::make_unique<KeyCatcher> ([this, commandID, row] (juce::KeyPress key)
                {
                    if (auto* mappings = commands.getKeyMappings())
                    {
                        // Take the key off whatever had it - two commands on one
                        // shortcut is never what anybody wanted.
                        const auto existing = mappings->findCommandForKeyPress (key);
                        if (existing != 0 && existing != commandID)
                            mappings->removeKeyPress (key);

                        mappings->addKeyPress (commandID, key, -1);
                        saveMappings();
                    }
                    list.repaintRow (row);
                });

                juce::CallOutBox::launchAsynchronously (std::move (catcher),
                                                        localAreaToGlobal (assignButton.getBounds()),
                                                        nullptr);
            }

            void applyPreset()
            {
                auto* mappings = commands.getKeyMappings();
                if (mappings == nullptr)
                    return;

                const juce::String preset = presetBox.getSelectedId() == 2 ? "ableton"
                                          : presetBox.getSelectedId() == 3 ? "cubase"
                                          : presetBox.getSelectedId() == 4 ? "studioone" : "scoresmith";
                ctx.settings->setKeymapPreset (preset);
                mappings->resetToDefaultMappings();

                // Only the handful of bindings that actually differ between the
                // hosts people migrate from; everything else keeps the default.
                struct Binding { juce::CommandID id; juce::KeyPress key; };
                std::vector<Binding> overrides;

                if (preset == "ableton")
                {
                    overrides.push_back ({ CommandIDs::transportRecord, juce::KeyPress ('9') });
                    overrides.push_back ({ CommandIDs::transportLoop,   juce::KeyPress ('l', juce::ModifierKeys::commandModifier, 0) });
                    overrides.push_back ({ CommandIDs::viewMixer,       juce::KeyPress (juce::KeyPress::tabKey) });
                }
                else if (preset == "cubase")
                {
                    overrides.push_back ({ CommandIDs::transportRecord,        juce::KeyPress (juce::KeyPress::numberPadMultiply) });
                    overrides.push_back ({ CommandIDs::transportReturnToStart, juce::KeyPress (juce::KeyPress::numberPadDecimalPoint) });
                    overrides.push_back ({ CommandIDs::viewMixer,              juce::KeyPress (juce::KeyPress::F3Key) });
                    overrides.push_back ({ CommandIDs::viewPianoRoll,          juce::KeyPress ('e', juce::ModifierKeys::commandModifier, 0) });
                }
                else if (preset == "studioone")
                {
                    overrides.push_back ({ CommandIDs::transportRecord, juce::KeyPress ('*') });
                    overrides.push_back ({ CommandIDs::viewMixer,       juce::KeyPress (juce::KeyPress::F3Key) });
                    overrides.push_back ({ CommandIDs::viewTimeline,    juce::KeyPress (juce::KeyPress::F2Key) });
                }

                for (const auto& binding : overrides)
                {
                    const auto existing = mappings->findCommandForKeyPress (binding.key);
                    if (existing != 0)
                        mappings->removeKeyPress (binding.key);
                    mappings->addKeyPress (binding.id, binding.key, -1);
                }

                saveMappings();
                list.repaint();
            }

            int getNumRows() override { return (int) visible.size(); }

            void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
            {
                if (row < 0 || row >= (int) visible.size())
                    return;

                if (selected)
                {
                    g.setColour (palette().accentDim);
                    g.fillRect (0, 0, width, height);
                }

                const auto* info = commands.getCommandForID (visible[(size_t) row]);
                if (info == nullptr) return;

                g.setColour (palette().textDim);
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (info->categoryName, 6, 0, 120, height, juce::Justification::centredLeft, true);

                g.setColour (palette().text);
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (info->shortName, 132, 0, width - 320, height, juce::Justification::centredLeft, true);

                juce::StringArray keys;
                if (auto* mappings = commands.getKeyMappings())
                    for (const auto& key : mappings->getKeyPressesAssignedToCommand (visible[(size_t) row]))
                        keys.add (key.getTextDescription());

                g.setColour (keys.isEmpty() ? palette().textDim : palette().accent);
                g.drawText (keys.joinIntoString (", "), width - 180, 0, 174, height,
                            juce::Justification::centredRight, true);
            }

            void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { assignToSelectedRow(); }

            AppContext& ctx;
            juce::ApplicationCommandManager& commands;
            std::vector<juce::CommandID> visible;
            juce::ListBox list;
            juce::TextEditor search;
            juce::ComboBox presetBox;
            juce::Label presetLabel;
            juce::TextButton assignButton, clearButton, resetButton;
        };

        //======================================================================
        // 7. Files and paths
        //======================================================================
        class FilesTab final : public juce::Component
        {
        public:
            explicit FilesTab (AppContext& c) : ctx (c)
            {
                addFolderRow (projectsLabel, projectsEditor, projectsButton, TRANS ("Projects folder"),
                              ctx.settings->getProjectsFolder(),
                              [this] (const juce::File& f) { ctx.settings->setProjectsFolder (f); });

                addFolderRow (cacheLabel, cacheEditor, cacheButton, TRANS ("Cache folder"),
                              ctx.settings->getCacheFolder(),
                              [this] (const juce::File& f) { ctx.settings->setCacheFolder (f); });

                addFolderRow (backupLabel, backupEditor, backupButton, TRANS ("Backup folder"),
                              ctx.settings->getBackupFolder(),
                              [this] (const juce::File& f) { ctx.settings->setBackupFolder (f); });

                clearCacheButton.setButtonText (TRANS ("Clear cache"));
                clearCacheButton.onClick = [this]
                {
                    const auto folder = ctx.settings->getCacheFolder();

                    // showOkCancelBox, not showAsync(MessageBoxOptions): its result
                    // convention (1 == the first button) is the documented one.
                    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
                                                        TRANS ("Clear cache"),
                                                        TRANS ("Delete everything in") + "\n"
                                                          + folder.getFullPathName() + "?",
                                                        TRANS ("Delete"), TRANS ("Cancel"), this,
                                                        juce::ModalCallbackFunction::create ([folder] (int result)
                    {
                        if (result != 1 || ! folder.isDirectory())
                            return;

                        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*",
                                                                                juce::File::findFilesAndDirectories))
                            entry.getFile().deleteRecursively();
                    }));
                };
                addAndMakeVisible (clearCacheButton);

                setUpLabel (*this, libraryLabel, TRANS ("Sample library folders (one per line)"));
                libraryEditor.setMultiLine (true, false);
                libraryEditor.setReturnKeyStartsNewLine (true);
                libraryEditor.setText (ctx.settings->getSampleLibraryFolders().joinIntoString ("\n"),
                                       juce::dontSendNotification);
                libraryEditor.onFocusLost = [this]
                {
                    juce::StringArray folders;
                    folders.addLines (libraryEditor.getText());
                    folders.removeEmptyStrings();
                    ctx.settings->setSampleLibraryFolders (folders);
                };
                addAndMakeVisible (libraryEditor);

                addLibraryButton.setButtonText (TRANS ("Add folder..."));
                addLibraryButton.onClick = [this]
                {
                    chooser = std::make_unique<juce::FileChooser> (TRANS ("Add a sample folder"));
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [this] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;

                        auto folders = ctx.settings->getSampleLibraryFolders();
                        folders.addIfNotAlreadyThere (folder.getFullPathName());
                        ctx.settings->setSampleLibraryFolders (folders);
                        libraryEditor.setText (folders.joinIntoString ("\n"), juce::dontSendNotification);
                    });
                };
                addAndMakeVisible (addLibraryButton);

                setUpLabel (*this, stemLabel, TRANS ("Stem separator executable"));
                stemEditor.setText (ctx.settings->getStemSeparatorExecutable().getFullPathName(),
                                    juce::dontSendNotification);
                stemEditor.onFocusLost = [this]
                {
                    ctx.settings->setStemSeparatorExecutable (juce::File (stemEditor.getText().trim()));
                };
                addAndMakeVisible (stemEditor);

                setUpLabel (*this, modelLabel, TRANS ("Transcription model folder"));
                modelEditor.setText (ctx.settings->getTranscriptionModelFolder().getFullPathName(),
                                     juce::dontSendNotification);
                modelEditor.onFocusLost = [this]
                {
                    ctx.settings->setTranscriptionModelFolder (juce::File (modelEditor.getText().trim()));
                };
                addAndMakeVisible (modelEditor);

                setUpLabel (*this, utauResamplerLabel, TRANS ("UTAU resampler executable"));
                utauResamplerEditor.setText (ctx.settings->getUtauResamplerExecutable().getFullPathName(),
                                            juce::dontSendNotification);
                utauResamplerEditor.onFocusLost = [this]
                {
                    ctx.settings->setUtauResamplerExecutable (juce::File (utauResamplerEditor.getText().trim()));
                };
                addAndMakeVisible (utauResamplerEditor);

                setUpLabel (*this, utauFoldersLabel, TRANS ("UTAU voicebank folders (one per line)"));
                utauFoldersEditor.setMultiLine (true, false);
                utauFoldersEditor.setReturnKeyStartsNewLine (true);
                utauFoldersEditor.setText (ctx.settings->getUtauVoicebankFolders().joinIntoString ("\n"),
                                          juce::dontSendNotification);
                utauFoldersEditor.onFocusLost = [this]
                {
                    juce::StringArray folders;
                    folders.addLines (utauFoldersEditor.getText());
                    folders.removeEmptyStrings();
                    ctx.settings->setUtauVoicebankFolders (folders);
                };
                addAndMakeVisible (utauFoldersEditor);

                addUtauFolderButton.setButtonText (TRANS ("Add folder..."));
                addUtauFolderButton.onClick = [this]
                {
                    chooser = std::make_unique<juce::FileChooser> (TRANS ("Add a UTAU voicebank folder"));
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [this] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;

                        auto folders = ctx.settings->getUtauVoicebankFolders();
                        folders.addIfNotAlreadyThere (folder.getFullPathName());
                        ctx.settings->setUtauVoicebankFolders (folders);
                        utauFoldersEditor.setText (folders.joinIntoString ("\n"), juce::dontSendNotification);
                    });
                };
                addAndMakeVisible (addUtauFolderButton);
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);

                auto folderRow = [&area] (juce::Label& l, juce::TextEditor& e, juce::TextButton& b)
                {
                    auto row = Row::next (area);
                    l.setBounds (row.removeFromLeft (labelWidth));
                    b.setBounds (row.removeFromRight (110));
                    row.removeFromRight (6);
                    e.setBounds (row);
                };

                folderRow (projectsLabel, projectsEditor, projectsButton);
                folderRow (cacheLabel, cacheEditor, cacheButton);
                folderRow (backupLabel, backupEditor, backupButton);

                clearCacheButton.setBounds (Row::next (area, 26).removeFromLeft (150));

                auto stemRow = Row::next (area);
                stemLabel.setBounds (stemRow.removeFromLeft (labelWidth));
                stemEditor.setBounds (stemRow);

                auto modelRow = Row::next (area);
                modelLabel.setBounds (modelRow.removeFromLeft (labelWidth));
                modelEditor.setBounds (modelRow);

                auto utauResamplerRow = Row::next (area);
                utauResamplerLabel.setBounds (utauResamplerRow.removeFromLeft (labelWidth));
                utauResamplerEditor.setBounds (utauResamplerRow);

                area.removeFromTop (6);
                auto utauFoldersHeader = Row::next (area, 22);
                utauFoldersLabel.setBounds (utauFoldersHeader.removeFromLeft (300));
                addUtauFolderButton.setBounds (utauFoldersHeader.removeFromRight (110));
                utauFoldersEditor.setBounds (Row::next (area, 80)); // fixed height - libraryEditor below still needs the rest

                area.removeFromTop (6);
                auto libraryHeader = Row::next (area, 22);
                libraryLabel.setBounds (libraryHeader.removeFromLeft (300));
                addLibraryButton.setBounds (libraryHeader.removeFromRight (110));
                libraryEditor.setBounds (area);
            }

        private:
            void addFolderRow (juce::Label& label, juce::TextEditor& editor, juce::TextButton& button,
                               const juce::String& text, const juce::File& initial,
                               std::function<void (const juce::File&)> apply)
            {
                setUpLabel (*this, label, text);
                editor.setText (initial.getFullPathName(), juce::dontSendNotification);
                editor.onFocusLost = [&editor, apply] { apply (juce::File (editor.getText().trim())); };
                addAndMakeVisible (editor);

                button.setButtonText (TRANS ("Browse..."));
                button.onClick = [this, &editor, apply, text]
                {
                    chooser = std::make_unique<juce::FileChooser> (text);
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [&editor, apply] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;
                        editor.setText (folder.getFullPathName(), juce::dontSendNotification);
                        apply (folder);
                    });
                };
                addAndMakeVisible (button);
            }

            AppContext& ctx;
            juce::Label projectsLabel, cacheLabel, backupLabel, libraryLabel, stemLabel, modelLabel;
            juce::TextEditor projectsEditor, cacheEditor, backupEditor, libraryEditor, stemEditor, modelEditor;
            juce::TextButton projectsButton, cacheButton, backupButton, clearCacheButton, addLibraryButton;
            juce::Label utauResamplerLabel, utauFoldersLabel;
            juce::TextEditor utauResamplerEditor, utauFoldersEditor;
            juce::TextButton addUtauFolderButton;
            std::unique_ptr<juce::FileChooser> chooser;
        };

        //======================================================================
        // 8. Account (spec 12: there is no licensing)
        //======================================================================
        class AccountTab final : public juce::Component
        {
        public:
            explicit AccountTab (AppContext& c) : ctx (c)
            {
                setUpLabel (*this, nameLabel, TRANS ("Display name"));
                nameEditor.setText (ctx.settings->raw().getValue ("account.displayName",
                                                                  juce::SystemStats::getFullUserName()),
                                    juce::dontSendNotification);
                nameEditor.onFocusLost = [this]
                {
                    ctx.settings->raw().setValue ("account.displayName", nameEditor.getText());
                };
                addAndMakeVisible (nameEditor);

                setUpLabel (*this, updateLabel, TRANS ("Update location"));
                updateEditor.setTextToShowWhenEmpty (TRANS ("shared folder or link where new builds appear"),
                                                     palette().textDim);
                updateEditor.setText (ctx.settings->raw().getValue ("account.updateLocation", {}),
                                      juce::dontSendNotification);
                updateEditor.onFocusLost = [this]
                {
                    ctx.settings->raw().setValue ("account.updateLocation", updateEditor.getText());
                };
                addAndMakeVisible (updateEditor);

                setUpNote (*this, versionLabel,
                           juce::String (JUCE_APPLICATION_NAME_STRING) + "  "
                             + juce::String (JUCE_APPLICATION_VERSION_STRING) + "\n"
                             + juce::SystemStats::getOperatingSystemName() + "\n"
                             + TRANS ("KANADE DAW is distributed free as a single edition. "
                                      "There are no plans, licences or subscriptions."));
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);
                layOutRow (area, nameLabel, nameEditor);
                layOutRow (area, updateLabel, updateEditor);
                area.removeFromTop (14);
                versionLabel.setBounds (area.removeFromTop (90));
            }

        private:
            AppContext& ctx;
            juce::Label nameLabel, updateLabel, versionLabel;
            juce::TextEditor nameEditor, updateEditor;
        };
    }

    //==========================================================================
    PreferencesDialog::PreferencesDialog (AppContext& c, juce::ApplicationCommandManager& cm,
                                          DarkLookAndFeel& laf, std::function<void()> onSettingsChanged)
        : ctx (c)
    {
        const auto tabColour = palette().panelBg;

        tabs.addTab (TRANS ("Audio"),          tabColour, new AudioTab (ctx, onSettingsChanged), true);
        tabs.addTab (TRANS ("MIDI"),           tabColour, new MidiTab (ctx), true);
        tabs.addTab (TRANS ("General"),        tabColour, new GeneralTab (ctx, laf, onSettingsChanged), true);
        tabs.addTab (TRANS ("Project"),        tabColour, new ProjectDefaultsTab (ctx), true);
        tabs.addTab (TRANS ("Plugins"),        tabColour, new PluginTab (ctx), true);
        tabs.addTab (TRANS ("Extensions"),     tabColour, new ExtensionsTab (ctx), true);
        tabs.addTab (TRANS ("Shortcuts"),      tabColour, new ShortcutTab (ctx, cm), true);
        tabs.addTab (TRANS ("Files"),          tabColour, new FilesTab (ctx), true);
        tabs.addTab (TRANS ("Account"),        tabColour, new AccountTab (ctx), true);

        tabs.setTabBarDepth (150);
        addAndMakeVisible (tabs);
        setSize (940, 620);
    }

    PreferencesDialog::~PreferencesDialog()
    {
        if (ctx.settings != nullptr)
            ctx.settings->flush();
    }

    void PreferencesDialog::launch (AppContext& ctx, juce::ApplicationCommandManager& cm,
                                    DarkLookAndFeel& laf, std::function<void()> onSettingsChanged)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new PreferencesDialog (ctx, cm, laf, std::move (onSettingsChanged)));
        options.dialogTitle            = TRANS ("Preferences");
        options.dialogBackgroundColour = palette().windowBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = true;
        options.launchAsync();
    }

    void PreferencesDialog::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void PreferencesDialog::resized()
    {
        tabs.setBounds (getLocalBounds());
    }
}
