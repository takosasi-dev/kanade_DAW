#include "Core/Types.h"

namespace ss
{
    double quantiseStepInBeats (Quantise q) noexcept
    {
        switch (q)
        {
            case Quantise::off:              return 0.0;
            case Quantise::whole:            return 4.0;
            case Quantise::half:             return 2.0;
            case Quantise::quarter:          return 1.0;
            case Quantise::eighth:           return 0.5;
            case Quantise::sixteenth:        return 0.25;
            case Quantise::thirtySecond:     return 0.125;
            case Quantise::eighthTriplet:    return 1.0 / 3.0;
            case Quantise::sixteenthTriplet: return 1.0 / 6.0;
        }
        return 0.0;
    }

    double applyQuantise (double beats, Quantise q, double strength) noexcept
    {
        const auto step = quantiseStepInBeats (q);
        if (step <= 0.0 || strength <= 0.0)
            return beats;

        const auto snapped = std::round (beats / step) * step;
        return beats + (snapped - beats) * juce::jlimit (0.0, 1.0, strength);
    }

    juce::String toString (Genre g)
    {
        switch (g)
        {
            case Genre::pop:        return "Pop";
            case Genre::rock:       return "Rock";
            case Genre::jazz:       return "Jazz";
            case Genre::lofi:       return "Lo-fi";
            case Genre::edm:        return "EDM";
            case Genre::orchestral: return "Orchestral";
            case Genre::cityPop:    return "City Pop";
            case Genre::ballad:     return "Ballad";
            case Genre::funk:       return "Funk";
            case Genre::bossaNova:  return "Bossa Nova";
        }
        return "Pop";
    }

    juce::String toString (TrackType t)
    {
        switch (t)
        {
            case TrackType::audio: return "audio";
            case TrackType::midi:  return "midi";
            case TrackType::utau:  return "utau";
        }
        return "midi";
    }

    juce::String toString (Quantise q)
    {
        switch (q)
        {
            case Quantise::off:              return "off";
            case Quantise::whole:            return "1/1";
            case Quantise::half:             return "1/2";
            case Quantise::quarter:          return "1/4";
            case Quantise::eighth:           return "1/8";
            case Quantise::sixteenth:        return "1/16";
            case Quantise::thirtySecond:     return "1/32";
            case Quantise::eighthTriplet:    return "1/8T";
            case Quantise::sixteenthTriplet: return "1/16T";
        }
        return "off";
    }

    const std::vector<Genre>& allGenres()
    {
        static const std::vector<Genre> genres {
            Genre::pop, Genre::rock, Genre::jazz, Genre::lofi, Genre::edm,
            Genre::orchestral, Genre::cityPop, Genre::ballad, Genre::funk, Genre::bossaNova
        };
        return genres;
    }

    Genre genreFromString (const juce::String& s)
    {
        for (auto g : allGenres())
            if (toString (g).equalsIgnoreCase (s))
                return g;
        return Genre::pop;
    }

    TrackType trackTypeFromString (const juce::String& s)
    {
        if (s.equalsIgnoreCase ("audio")) return TrackType::audio;
        if (s.equalsIgnoreCase ("utau"))  return TrackType::utau;
        return TrackType::midi;
    }

    Quantise quantiseFromString (const juce::String& s)
    {
        for (int i = 0; i <= (int) Quantise::sixteenthTriplet; ++i)
            if (toString ((Quantise) i) == s)
                return (Quantise) i;
        return Quantise::off;
    }
}
