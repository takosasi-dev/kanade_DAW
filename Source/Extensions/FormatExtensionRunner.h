#pragma once
#include "Core/Project.h"
#include "Extensions/FormatExtension.h"
#include "Plugins/PluginManager.h"
#include <juce_core/juce_core.h>
#include <functional>
#include <map>

namespace ss
{
    /** Runs one format extension's executable. Abstracted so tests never
        need a real extension executable on disk - mirrors
        Vocal/UtauRenderer.h's ResamplerRunner/ExternalResamplerRunner
        split exactly. */
    class FormatExtensionRunner
    {
    public:
        virtual ~FormatExtensionRunner() = default;

        /** args[0] is the executable itself (juce::ChildProcess::start
            convention). `timeoutMs` < 0 means no timeout (customUI
            extensions - the caller relies on `shouldCancel` instead).
            `onProgress` (0.0-1.0) is called from a background thread
            whenever the process prints a `PROGRESS:<0-100>` line - see
            parseProgressLine below; either callback may be null. Returns
            true only if the process exited 0 AND expectedOutput now
            exists; on failure, errorOut carries whatever the process
            wrote (that wasn't a PROGRESS: line) to stderr/stdout (or a
            generic timeout/exit-code message when that was empty). A
            true `shouldCancel()` result kills the process and returns
            false with an empty errorOut - the caller is expected to
            recognise its own cancellation and not treat that as a
            failure (see TaskPanel::isCancelled()). */
        virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                           int timeoutMs,
                           std::function<void (float)> onProgress,
                           std::function<bool()> shouldCancel,
                           juce::String& errorOut) = 0;
    };

    /** Production FormatExtensionRunner - shells out via juce::ChildProcess,
        exactly like ExternalResamplerRunner does for the UTAU resampler. */
    class ExternalFormatExtensionRunner final : public FormatExtensionRunner
    {
    public:
        bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                   int timeoutMs,
                   std::function<void (float)> onProgress,
                   std::function<bool()> shouldCancel,
                   juce::String& errorOut) override;
    };

    /** Parses one line of an extension's stdout/stderr for the
        `PROGRESS:<0-100>` convention (Format Extension Settings/Progress/
        Custom UI design, docs/superpowers/specs/
        2026-08-31-extension-settings-ui-design.md). Returns true and sets
        `out` (0.0-1.0, clamped) only for a well-formed match; any other
        line - including a malformed "PROGRESS:" line - returns false and
        leaves `out` untouched. Exposed here (not file-local) so
        FormatExtensionTests.cpp can test the parsing contract directly,
        without spawning a real process. */
    bool parseProgressLine (const juce::String& line, float& out);
}

namespace ss::io
{
    /** Export: writes `project` to a temp .dawproject, sets each
        `additionalInputs` entry as an environment variable on the KANADE
        DAW process for the duration of the call (cleared again before
        returning, success or failure), invokes the extension with
        `--export <temp.dawproject> <outputFile>` via `runner`, and reports
        failure via errorOut/warningsOut using the same two-signal contract
        exportDawProject already uses. `onProgress`/`shouldCancel` are
        forwarded to `runner.run` unchanged (both default to null - most
        callers, and every existing test, don't care). The timeout passed
        to the runner is computed here: -1 (no timeout) when
        `extension.customUI` is true, the usual 120s constant otherwise -
        callers never choose it directly. The temp .dawproject is always
        deleted before returning, success or not. */
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    const std::map<juce::String, juce::String>& additionalInputs,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut,
                                    std::function<void (float)> onProgress = nullptr,
                                    std::function<bool()> shouldCancel = nullptr);

    /** Import: invokes the extension with
        `--import <inputFile> <temp.dawproject>` via `runner` (always with
        the fixed 120s timeout and no progress/cancel callbacks - see the
        Import scope note in docs/superpowers/specs/
        2026-08-31-extension-settings-ui-design.md), then imports the
        extension-produced .dawproject into `project` via importDawProject.
        The temp .dawproject is always deleted before returning, success
        or not. */
    bool runFormatExtensionImport (const FormatExtension& extension, Project& project,
                                    PluginManager& plugins, const juce::File& inputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);
}
