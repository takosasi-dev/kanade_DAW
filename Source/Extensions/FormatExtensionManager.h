#pragma once
#include "Extensions/FormatExtension.h"
#include <vector>

namespace ss
{
    /** Discovers format extensions across a set of user-configured folders.
        See docs/superpowers/specs/2026-08-28-format-extension-api-design.md. */
    class FormatExtensionManager
    {
    public:
        /** Rescans every immediate subfolder of every path in scanPaths for
            a manifest.json. Replaces the previous discovery list wholesale.
            A subfolder with no manifest.json is silently not an extension;
            one with a manifest.json that fails to parse adds one
            warningsOut entry and is otherwise skipped - one bad folder
            never blocks discovering the rest. */
        void rescan (const juce::StringArray& scanPaths, juce::StringArray& warningsOut);

        const std::vector<FormatExtension>& getExtensions() const noexcept { return extensions; }

        /** Extensions usable for `wanted` (importOnly or exportOnly): an
            extension whose own direction is `both` matches either query,
            one whose direction is `importOnly` matches only `importOnly`,
            etc. Passing `both` returns every extension unfiltered. */
        std::vector<const FormatExtension*> matching (ExtensionDirection wanted) const;

    private:
        std::vector<FormatExtension> extensions;
    };
}
