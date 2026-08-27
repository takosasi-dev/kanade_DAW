#include "Core/AppContext.h"
#include "Core/Project.h"
#include "Core/Settings.h"
#include "Engine/AudioEngine.h"
#include "Plugins/PluginManager.h"
#include "AI/Transcriber.h"
#include "AI/Generator.h"

namespace ss
{
    AppContext::AppContext()
    {
        settings      = std::make_unique<Settings>();
        plugins       = std::make_unique<PluginManager> (*settings);
        engine        = std::make_unique<AudioEngine> (*settings, *plugins);
        transcriber   = std::make_unique<Transcriber>();
        generator     = std::make_unique<Generator>();
        stemSeparator = std::make_unique<StemSeparator> (*settings);

        auto blank = std::make_unique<Project>();
        blank->sampleRate = settings->getDefaultSampleRate();
        blank->bitDepth   = settings->getDefaultBitDepth();
        blank->tempo.setEvents ({ { 0.0, settings->getDefaultBpm() } });
        blank->addTrack (TrackType::audio, "Audio 1");
        blank->addTrack (TrackType::midi,  "MIDI 1");
        blank->clearDirty();

        setProject (std::move (blank));

        /*  Nothing else opens the device: the engine exposes the call, but it has
            to be driven from here, after the project exists so the mixer graph is
            in place.  A failure is not fatal - Preferences still lets the user
            pick a working device - but it has to be visible rather than silently
            leaving the whole app mute.                                         */
        if (const auto error = engine->initialiseAudioDevice(); error.isNotEmpty())
        {
            audioDeviceError = error;
            juce::Logger::writeToLog ("audio: FAILED to open a device - " + error);
        }
        else if (auto* device = engine->getDeviceManager().getCurrentAudioDevice())
        {
            juce::Logger::writeToLog ("audio: " + device->getTypeName() + " / " + device->getName()
                                      + " @ " + juce::String (device->getCurrentSampleRate(), 0) + " Hz, buffer "
                                      + juce::String (device->getCurrentBufferSizeSamples()) + " ("
                                      + juce::String (engine->getLatencyMs(), 1) + " ms), in "
                                      + juce::String (device->getActiveInputChannels().countNumberOfSetBits())
                                      + " / out "
                                      + juce::String (device->getActiveOutputChannels().countNumberOfSetBits()));
        }
        else
        {
            juce::Logger::writeToLog ("audio: initialise reported success but no device is open");
        }
    }

    AppContext::~AppContext()
    {
        if (engine != nullptr)
            engine->saveAudioDeviceState();

        // Tear down in reverse dependency order: the engine holds pointers into
        // the project and instances handed out by the plugin manager.
        engine.reset();
        project.reset();
        stemSeparator.reset();
        generator.reset();
        transcriber.reset();
        plugins.reset();
        settings.reset();
    }

    void AppContext::setProject (std::unique_ptr<Project> newProject)
    {
        engine->setProject (nullptr);       // stop touching the old document first
        project = std::move (newProject);
        engine->setProject (project.get());
    }
}
