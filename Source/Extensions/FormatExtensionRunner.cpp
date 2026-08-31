#include "Extensions/FormatExtensionRunner.h"
#include "IO/DawProject.h"
#include <cstdlib>

namespace ss
{
    namespace
    {
        constexpr int extensionTimeoutMs = 120000;
        constexpr int pollIntervalMs = 50;

        void setEnvVar (const juce::String& name, const juce::String& value)
        {
           #if JUCE_WINDOWS
            _wputenv_s (name.toWideCharPointer(), value.toWideCharPointer());
           #else
            setenv (name.toRawUTF8(), value.toRawUTF8(), 1);
           #endif
        }

        void clearEnvVar (const juce::String& name)
        {
           #if JUCE_WINDOWS
            _wputenv_s (name.toWideCharPointer(), L"");
           #else
            unsetenv (name.toRawUTF8());
           #endif
        }

        /** Sets every entry in `vars` as an environment variable on this
            process for its own scope's lifetime, clearing all of them again
            on destruction - RAII so a return/throw partway through
            runFormatExtensionExport can't leak one. */
        class ScopedEnvVars
        {
        public:
            explicit ScopedEnvVars (const std::map<juce::String, juce::String>& vars)
            {
                for (const auto& [name, value] : vars)
                {
                    setEnvVar (name, value);
                    names.push_back (name);
                }
            }

            ~ScopedEnvVars()
            {
                for (const auto& name : names)
                    clearEnvVar (name);
            }

            ScopedEnvVars (const ScopedEnvVars&) = delete;
            ScopedEnvVars& operator= (const ScopedEnvVars&) = delete;

        private:
            std::vector<juce::String> names;
        };

        /** Drains a running juce::ChildProcess's combined stdout/stderr on
            its own thread for as long as the process lives, so a chatty
            extension can never block on a full pipe waiting for someone to
            read it. Splits the stream into lines; a line matching
            parseProgressLine's PROGRESS: convention calls `onProgress` and
            is otherwise discarded, every other line is kept for
            nonProgressText(). */
        class OutputReader final : private juce::Thread
        {
        public:
            OutputReader (juce::ChildProcess& processIn, std::function<void (float)> onProgressIn)
                : juce::Thread ("FormatExtensionOutputReader"),
                  process (processIn), onProgress (std::move (onProgressIn))
            {
                startThread();
            }

            /** Blocks until the reader has drained everything the process
                will ever write (i.e. the process's output handle has
                closed). Callers ensure the process is already dead first -
                that's what makes the underlying blocking read return -
                and must call this, and let it return, before trusting the
                process's exit code or nonProgressText(). */
            void waitUntilDone()
            {
                stopThread (2000);
            }

            juce::String nonProgressText() const
            {
                const juce::ScopedLock sl (lock);
                return accumulated.trim();
            }

        private:
            void run() override
            {
                char buffer[4096];
                juce::String pending;

                for (;;)
                {
                    // 1 byte, not sizeof (buffer): readProcessOutput loops
                    // internally until it has the FULL requested count (or
                    // the process exits), so asking for 4096 would sit on a
                    // 13-byte "PROGRESS:42" line until the run was nearly
                    // over. Asking for 1 returns as soon as anything is
                    // available; JUCE already sleeps 1ms when there is
                    // nothing, so this doesn't spin.
                    const auto bytesRead = process.readProcessOutput (buffer, 1);
                    if (bytesRead <= 0)
                        break;   // pipe closed - the process has exited (or is exiting)

                    pending += juce::String::fromUTF8 (buffer, bytesRead);

                    int newline;
                    while ((newline = pending.indexOfChar ('\n')) >= 0)
                    {
                        handleLine (pending.substring (0, newline).trimEnd());
                        pending = pending.substring (newline + 1);
                    }
                }

                if (pending.isNotEmpty())
                    handleLine (pending);
            }

            void handleLine (const juce::String& line)
            {
                float progress = 0.0f;
                if (parseProgressLine (line, progress))
                {
                    if (onProgress)
                        onProgress (progress);
                    return;
                }

                const juce::ScopedLock sl (lock);
                if (accumulated.isNotEmpty())
                    accumulated += "\n";
                accumulated += line;
            }

            juce::ChildProcess& process;
            std::function<void (float)> onProgress;
            mutable juce::CriticalSection lock;
            juce::String accumulated;
        };
    }

    bool parseProgressLine (const juce::String& line, float& out)
    {
        static const juce::String prefix ("PROGRESS:");
        if (! line.startsWith (prefix))
            return false;

        const auto numberText = line.substring (prefix.length()).trim();
        if (numberText.isEmpty() || ! numberText.containsOnly ("0123456789."))
            return false;

        out = juce::jlimit (0.0f, 1.0f, numberText.getFloatValue() / 100.0f);
        return true;
    }

    bool ExternalFormatExtensionRunner::run (const juce::StringArray& args, const juce::File& expectedOutput,
                                              int timeoutMs,
                                              std::function<void (float)> onProgress,
                                              std::function<bool()> shouldCancel,
                                              juce::String& errorOut)
    {
        juce::ChildProcess process;
        if (! process.start (args))
        {
            errorOut = "Could not start \"" + args[0] + "\".";
            return false;
        }

        OutputReader reader (process, std::move (onProgress));

        const bool hasDeadline = timeoutMs >= 0;
        // Elapsed-since-start rather than an absolute deadline: the
        // millisecond counter wraps every ~49.7 days of uptime, and an
        // overflowed deadline would fire on the very next check. Unsigned
        // subtraction is modular, so it stays correct across the wrap.
        const auto startTime = juce::Time::getMillisecondCounter();
        bool cancelled = false;
        bool timedOut = false;

        while (process.isRunning())
        {
            if (shouldCancel && shouldCancel())
            {
                process.kill();
                cancelled = true;
                break;
            }

            if (hasDeadline && (juce::Time::getMillisecondCounter() - startTime) > (juce::uint32) timeoutMs)
            {
                process.kill();
                timedOut = true;
                break;
            }

            juce::Thread::sleep (pollIntervalMs);
        }

        process.waitForProcessToFinish (2000);
        reader.waitUntilDone();

        if (cancelled)
        {
            errorOut = {};
            return false;
        }

        if (timedOut)
        {
            errorOut = "\"" + args[0] + "\" timed out after "
                       + juce::String (timeoutMs / 1000) + "s.";
            return false;
        }

        const auto exitCode = process.getExitCode();
        const auto output = reader.nonProgressText();

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
                                    const std::map<juce::String, juce::String>& additionalInputs,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut,
                                    std::function<void (float)> onProgress,
                                    std::function<bool()> shouldCancel)
    {
        const auto tempDawProject = juce::File::createTempFile ("dawproject");

        if (! exportDawProject (tempDawProject, project, plugins, errorOut, warningsOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const juce::StringArray args { extension.executable.getFullPathName(), "--export",
                                        tempDawProject.getFullPathName(), outputFile.getFullPathName() };

        const int timeoutMs = extension.customUI ? -1 : extensionTimeoutMs;

        bool ok = false;
        {
            const ScopedEnvVars scopedVars (additionalInputs);
            ok = runner.run (args, outputFile, timeoutMs, std::move (onProgress), std::move (shouldCancel), errorOut);
        }

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
        if (! runner.run (args, tempDawProject, extensionTimeoutMs, nullptr, nullptr, errorOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const bool ok = importDawProject (tempDawProject, project, plugins, errorOut, warningsOut);
        tempDawProject.deleteFile();
        return ok;
    }
}
