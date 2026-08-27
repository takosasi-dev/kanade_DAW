#pragma once
#include "Core/Project.h"
#include <juce_dsp/juce_dsp.h>

namespace ss
{
    /** Base for the built-in effect suite (spec 8.4.6).
        Deliberately not a juce::AudioProcessor: these are fixed, internal and
        never need a generic parameter host - a plain process() plus a
        NamedValueSet of parameters is the whole contract.                     */
    class BuiltinEffect
    {
    public:
        virtual ~BuiltinEffect() = default;

        virtual juce::String getType() const = 0;
        virtual void prepare (double sampleRate, int maxBlockSize, int numChannels) = 0;
        virtual void reset() = 0;
        virtual void process (juce::AudioBuffer<float>&) = 0;

        /** Parameter ids, defaults and ranges, for the generic editor UI. */
        struct ParamInfo { juce::String id, label; float min, max, defaultValue; juce::String suffix; bool logarithmic = false; };
        virtual std::vector<ParamInfo> getParameterInfo() const = 0;

        virtual void setParameters (const juce::NamedValueSet&) = 0;
        virtual juce::NamedValueSet getParameters() const = 0;

        /** [audio thread] Sets one parameter from an automation lane, where
            `normalised` is 0..1 across that parameter's own min..max (spec 8.4.5).
            Never blocks: a value that collides with a message-thread write is
            dropped and the next block writes the newer one. */
        virtual void setParameterNormalised (int /*index*/, float /*normalised*/) noexcept {}

        /** Extra readouts for the UI (gain reduction, spectrum, LUFS...). */
        virtual float getMeter (const juce::String& /*meterId*/) const { return 0.0f; }

        /** Pushed by the mixer when the project tempo changes.  Only the
            tempo-syncable effects (delay) care; everything else ignores it. */
        virtual void setTempoBpm (double /*bpm*/) {}

        bool bypassed = false;
    };

    /** Ids: "eq" "compressor" "reverb" "delay" "limiter" "gate" "saturator" "loudnessMeter" */
    std::unique_ptr<BuiltinEffect> createBuiltinEffect (const juce::String& type);
    juce::StringArray getBuiltinEffectTypes();
    juce::String getBuiltinEffectDisplayName (const juce::String& type);
}
