#pragma once
#include <juce_core/juce_core.h>
#include <memory>

namespace ss
{
    class Project;  class Settings;
    class AudioEngine;  class PluginManager;
    class Transcriber;  class Generator;  class StemSeparator;
    class FormatExtensionManager;

    /** Everything the app owns, in one place, handed to the UI by reference.
        Constructed once in Main.cpp; there is no singleton and no service
        locator - views that need a subsystem take a reference to this.        */
    struct AppContext
    {
        AppContext();
        ~AppContext();

        std::unique_ptr<Settings>      settings;
        std::unique_ptr<Project>       project;
        std::unique_ptr<AudioEngine>   engine;
        std::unique_ptr<PluginManager> plugins;
        std::unique_ptr<Transcriber>   transcriber;
        std::unique_ptr<Generator>     generator;
        std::unique_ptr<StemSeparator> stemSeparator;
        std::unique_ptr<FormatExtensionManager> formatExtensions;

        /** Empty when the audio device opened; the driver's message otherwise. */
        juce::String audioDeviceError;

        /** Replaces the current document and re-points every subsystem at it. */
        void setProject (std::unique_ptr<Project>);
    };
}
