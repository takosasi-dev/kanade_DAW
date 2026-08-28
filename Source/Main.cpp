#include "Core/AppContext.h"
#include "Core/Settings.h"
#include "Core/Localisation.h"
#include "Plugins/PluginManager.h"
#include "UI/MainComponent.h"
#include "UI/WhatsNewDialog.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace ss
{
    class ScoreSmithApplication : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override       { return "KANADE DAW"; }
        const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
        bool moreThanOneInstanceAllowed() override             { return true; }

        void initialise (const juce::String& commandLine) override
        {
            /*  The plugin scanner relaunches this same binary as a worker process
                (spec 8.4.2 / 15.6).  The worker must keep running the message
                loop - it loads plugins on the message thread and quits itself
                when the coordinator disconnects - so all we do here is skip
                every bit of UI setup.  Calling quit() would kill the scan.     */
            if (PluginManager::runScannerProcessIfRequested (commandLine))
                return;

            /*  Gap 16 (spec 10.3/15.6): each hosted plugin instance is relaunched
                as its own worker process, so a crash while playing takes that
                process down, not the DAW. Same shape as the scanner dispatch
                above - skip UI setup, keep pumping the message loop.           */
            if (PluginManager::runPluginHostProcessIfRequested (commandLine))
                return;

            /*  --run-tests[=Category] runs the juce::UnitTest suites and quits.
                This is a GUI-subsystem binary with no stdout, so the report goes
                to a file next to the executable and the pass/fail comes back as
                the process exit code - which is what a build script needs.     */
            if (commandLine.contains ("--run-tests"))
            {
                runUnitTests (commandLine);
                return;
            }

            /*  A DAW that misbehaves on someone else's machine is undebuggable
                without this, and a GUI-subsystem binary has nowhere else to put
                diagnostics.  Lives in the user's app-data folder.              */
            logger.reset (juce::FileLogger::createDefaultAppLogger ("KANADE DAW", "KANADE DAW.log",
                                                                    "KANADE DAW " + getApplicationVersion()));
            juce::Logger::setCurrentLogger (logger.get());

            context = std::make_unique<AppContext>();
            setUiLanguage (context->settings->getLanguage());

            mainWindow = std::make_unique<MainWindow> (getApplicationName(), *context);

            /*  Show once per update, never on a fresh install (see
                WhatsNew::shouldShow). Recording the version happens either way -
                a fresh install has nothing to show but still needs to stop
                looking "unseen" for the next launch. callAsync so the dialog
                attaches after MainWindow is fully constructed and visible,
                rather than racing its construction.                            */
            {
                const auto currentVersion = getApplicationVersion();
                const auto lastSeenVersion = context->settings->getLastSeenVersion();
                context->settings->setLastSeenVersion (currentVersion);

                if (WhatsNew::shouldShow (lastSeenVersion, currentVersion))
                    juce::MessageManager::callAsync ([currentVersion]
                    {
                        WhatsNewDialog::launchForVersion (currentVersion);
                    });
            }
        }

        void shutdown() override
        {
            mainWindow.reset();
            context.reset();
            juce::Logger::setCurrentLogger (nullptr);
            logger.reset();
        }

        void systemRequestedQuit() override
        {
            if (mainWindow == nullptr)
            {
                quit();
                return;
            }

            mainWindow->getMainComponent().tryToQuit ([this] (bool shouldQuit)
            {
                if (shouldQuit)
                    quit();
            });
        }

    private:
        /** Collects the runner's log so it can be written out in one go. */
        class FileReportingRunner : public juce::UnitTestRunner
        {
        public:
            juce::StringArray lines;

        private:
            void logMessage (const juce::String& message) override { lines.add (message); }
        };

        void runUnitTests (const juce::String& commandLine)
        {
            juce::String category;

            if (const auto eq = commandLine.fromFirstOccurrenceOf ("--run-tests=", false, false);
                eq.isNotEmpty())
                category = eq.upToFirstOccurrenceOf (" ", false, false).unquoted();

            FileReportingRunner runner;
            runner.setAssertOnFailure (false);
            runner.runTestsInCategory (category);   // empty category == run everything

            int failures = 0, passes = 0;

            for (int i = 0; i < runner.getNumResults(); ++i)
            {
                const auto* r = runner.getResult (i);
                failures += r->failures;
                passes   += r->passes;
            }

            runner.lines.add ({});
            runner.lines.add ("TOTAL: " + juce::String (passes) + " passed, "
                              + juce::String (failures) + " failed");

            const auto report = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                    .getSiblingFile ("test-results.txt");
            report.replaceWithText (runner.lines.joinIntoString (juce::newLine));

            setApplicationReturnValue (failures > 0 ? 1 : 0);
            quit();
        }

        class MainWindow : public juce::DocumentWindow
        {
        public:
            MainWindow (const juce::String& windowName, AppContext& ctx)
                : DocumentWindow (windowName,
                                  juce::Desktop::getInstance().getDefaultLookAndFeel()
                                      .findColour (juce::ResizableWindow::backgroundColourId),
                                  DocumentWindow::allButtons)
            {
                setUsingNativeTitleBar (true);
                auto* content = new MainComponent (ctx);
                setContentOwned (content, true);
                setMenuBar (content);
                setResizable (true, false);
                setResizeLimits (1024, 640, 30000, 30000);
                centreWithSize (1600, 960);
                setVisible (true);
            }

            ~MainWindow() override
            {
                setMenuBar (nullptr);
            }

            MainComponent& getMainComponent()
            {
                return *static_cast<MainComponent*> (getContentComponent());
            }

            void closeButtonPressed() override
            {
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
        };

        std::unique_ptr<juce::FileLogger> logger;
        std::unique_ptr<AppContext> context;
        std::unique_ptr<MainWindow> mainWindow;
    };
}

START_JUCE_APPLICATION (ss::ScoreSmithApplication)
