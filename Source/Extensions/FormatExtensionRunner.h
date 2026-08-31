#pragma once
#include "Core/Project.h"
#include "Extensions/FormatExtension.h"
#include "Plugins/PluginManager.h"
#include <juce_core/juce_core.h>

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
            convention). Returns true only if the process exited 0 AND
            expectedOutput now exists; on failure, errorOut carries
            whatever the process wrote to stderr/stdout (or a generic
            timeout/exit-code message when that was empty). */
        virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                           juce::String& errorOut) = 0;
    };

    /** Production FormatExtensionRunner - shells out via juce::ChildProcess,
        exactly like ExternalResamplerRunner does for the UTAU resampler. */
    class ExternalFormatExtensionRunner final : public FormatExtensionRunner
    {
    public:
        bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                   juce::String& errorOut) override;
    };
}

namespace ss::io
{
    /** Export: writes `project` to a temp .dawproject, invokes the
        extension with `--export <temp.dawproject> <outputFile>` via
        `runner`, and reports failure via errorOut/warningsOut using the
        same two-signal contract exportDawProject already uses. The temp
        .dawproject is always deleted before returning, success or not. */
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);

    /** Import: invokes the extension with
        `--import <inputFile> <temp.dawproject>` via `runner`, then imports
        the extension-produced .dawproject into `project` via
        importDawProject. The temp .dawproject is always deleted before
        returning, success or not. */
    bool runFormatExtensionImport (const FormatExtension& extension, Project& project,
                                    PluginManager& plugins, const juce::File& inputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);
}
