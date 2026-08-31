#include "Extensions/FormatExtensionRunner.h"
#include "IO/DawProject.h"

namespace ss
{
    namespace
    {
        constexpr int extensionTimeoutMs = 120000;
    }

    bool ExternalFormatExtensionRunner::run (const juce::StringArray& args, const juce::File& expectedOutput,
                                              juce::String& errorOut)
    {
        // ponytail: reads output only after the process exits (via
        // waitForProcessToFinish, which polls isRunning() on its own timer and
        // reliably honours the timeout - juce::Time::getMillisecondCounter()
        // never runs, since nothing here waits on it) rather than draining the
        // pipe while polling. A drain-while-waiting attempt was tried and
        // reverted: juce::ChildProcess::readProcessOutput() blocks internally
        // until either the requested byte count arrives or the process exits
        // (see juce_Threads_windows.cpp's ActiveProcess::read), so calling it
        // from inside the wait loop can itself block past the timeout deadline
        // for a quiet/hung extension - defeating the very thing the timeout
        // exists for. A chatty extension that fills the pipe and blocks on its
        // own write() still gets killed correctly at the timeout (JUCE's
        // waitForProcessToFinish doesn't depend on the pipe being drained) -
        // the only cost is a less specific "timed out" message instead of
        // whatever it was printing. Fix properly (drain on a dedicated reader
        // thread) if that message accuracy ever matters enough to justify it.
        juce::ChildProcess process;
        if (! process.start (args))
        {
            errorOut = "Could not start \"" + args[0] + "\".";
            return false;
        }

        if (! process.waitForProcessToFinish (extensionTimeoutMs))
        {
            process.kill();
            errorOut = "\"" + args[0] + "\" timed out after "
                       + juce::String (extensionTimeoutMs / 1000) + "s.";
            return false;
        }

        const auto exitCode = process.getExitCode();
        const auto output = process.readAllProcessOutput().trim();

        if (exitCode != 0)
        {
            errorOut = output.isNotEmpty()
                          ? output
                          : ("\"" + args[0] + "\" exited with code " + juce::String (exitCode) + ".");
            return false;
        }

        if (! expectedOutput.existsAsFile())
        {
            errorOut = "\"" + args[0] + "\" exited successfully but did not produce \""
                       + expectedOutput.getFullPathName() + "\".";
            return false;
        }

        return true;
    }
}

namespace ss::io
{
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut)
    {
        const auto tempDawProject = juce::File::createTempFile ("dawproject");

        if (! exportDawProject (tempDawProject, project, plugins, errorOut, warningsOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const juce::StringArray args { extension.executable.getFullPathName(), "--export",
                                        tempDawProject.getFullPathName(), outputFile.getFullPathName() };
        const bool ok = runner.run (args, outputFile, errorOut);
        tempDawProject.deleteFile();
        return ok;
    }

    bool runFormatExtensionImport (const FormatExtension& extension, Project& project,
                                    PluginManager& plugins, const juce::File& inputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut)
    {
        const auto tempDawProject = juce::File::createTempFile ("dawproject");

        const juce::StringArray args { extension.executable.getFullPathName(), "--import",
                                        inputFile.getFullPathName(), tempDawProject.getFullPathName() };
        if (! runner.run (args, tempDawProject, errorOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const bool ok = importDawProject (tempDawProject, project, plugins, errorOut, warningsOut);
        tempDawProject.deleteFile();
        return ok;
    }
}
