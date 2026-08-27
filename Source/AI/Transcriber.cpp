#include "AI/Transcriber.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

/*  Audio -> MIDI (spec 8.2).

    The pipeline is: STFT front end -> spectral-flux onsets with an adaptive
    median threshold -> pitch tracking (YIN for monophonic, harmonic-sum
    salience for polyphonic) -> note segmentation -> tempo/swing -> quantise.

    Everything here is classical DSP.  Spec 10.2 wants a CNN/CRNN (Basic Pitch /
    MT3 lineage) eventually; that model would replace `trackPitches()` and
    `polyphonicFrames()` and feed the same segmentation and confidence plumbing,
    so the note-level contract - and the whole confidence UI in 9.3 - does not
    change when it lands.                                                       */

namespace ss
{

namespace
{
    //==========================================================================
    //  Tunables.  These are calibration knobs, not constants of nature - real
    //  recordings need them nudged, so they live in one place.
    //==========================================================================
    constexpr int    kFftOrder            = 11;            // 2048 bins @ ~21 Hz
    constexpr int    kFftSize             = 1 << kFftOrder;
    constexpr double kHopSeconds          = 0.010;         // 10 ms, spec 8.2
    // ponytail: fixed 46 ms YIN window, so fast runs smear and nothing below
    // ~30 Hz is findable; make it pitch-adaptive (or multi-resolution) if bass
    // and piccolo ever need to be accurate in the same pass.
    constexpr int    kYinWindow           = 2048;          // 46 ms @ 44.1k
    constexpr int    kYinFftOrder         = 12;            // 2 * kYinWindow
    constexpr int    kYinFftSize          = 1 << kYinFftOrder;
    constexpr double kYinThreshold        = 0.15;          // YIN "absolute threshold"
    constexpr float  kSilenceRms          = 1.0e-4f;       // ~-80 dBFS
    constexpr double kOnsetBackoffSeconds = 0.010;         // flux peaks lag the transient
    constexpr float  kPolyReliability     = 0.60f;         // spec 15.2 - the ceiling is real
    constexpr int    kNumHarmonics        = 8;
    constexpr int    kMaxPolyVoices       = 5;

    template <typename T>
    T medianOf (std::vector<T> v)
    {
        if (v.empty())
            return T();

        const auto mid = (std::ptrdiff_t) (v.size() / 2);
        std::nth_element (v.begin(), v.begin() + mid, v.end());
        return v[(size_t) mid];
    }

    double midiToHz (double midi) noexcept { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }
    double hzToMidi (double hz)   noexcept { return 69.0 + 12.0 * std::log2 (juce::jmax (1.0e-6, hz) / 440.0); }

    //==========================================================================
    //  STFT front end
    //==========================================================================
    struct Spectrogram
    {
        std::vector<float> mags;      // numFrames * numBins, row major
        int    numFrames = 0;
        int    numBins   = 0;
        int    hop       = 1;
        double frameRate = 0.0;       // frames per second
        double binHz     = 0.0;

        const float* frame (int i) const noexcept
        {
            return mags.data() + (size_t) i * (size_t) numBins;
        }
    };

    Spectrogram computeStft (const std::vector<float>& mono, double sampleRate, int hop)
    {
        Spectrogram s;
        s.numBins   = kFftSize / 2 + 1;
        s.hop       = hop;
        s.frameRate = sampleRate / (double) hop;
        s.binHz     = sampleRate / (double) kFftSize;

        if (mono.size() < (size_t) kFftSize)
            return s;

        s.numFrames = (int) ((mono.size() - (size_t) kFftSize) / (size_t) hop) + 1;
        s.mags.resize ((size_t) s.numFrames * (size_t) s.numBins);

        juce::dsp::FFT fft (kFftOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) kFftSize,
                                                    juce::dsp::WindowingFunction<float>::hann,
                                                    false);

        std::vector<float> scratch ((size_t) kFftSize * 2, 0.0f);

        for (int f = 0; f < s.numFrames; ++f)
        {
            std::fill (scratch.begin(), scratch.end(), 0.0f);

            const auto offset = (size_t) f * (size_t) hop;
            std::copy (mono.begin() + (std::ptrdiff_t) offset,
                       mono.begin() + (std::ptrdiff_t) (offset + (size_t) kFftSize),
                       scratch.begin());

            window.multiplyWithWindowingTable (scratch.data(), (size_t) kFftSize);
            fft.performFrequencyOnlyForwardTransform (scratch.data(), true);

            std::copy (scratch.begin(), scratch.begin() + s.numBins,
                       s.mags.begin() + (std::ptrdiff_t) ((size_t) f * (size_t) s.numBins));
        }

        return s;
    }

    //==========================================================================
    //  Onsets: spectral flux + adaptive median threshold
    //==========================================================================

    /** Half-wave rectified, log-compressed spectral flux, normalised to 0..1.
        Log compression is what makes a quiet note in a loud passage register at
        all - raw magnitude flux only ever finds the loudest events.            */
    std::vector<float> spectralFlux (const Spectrogram& s)
    {
        std::vector<float> flux ((size_t) juce::jmax (0, s.numFrames), 0.0f);

        constexpr float gamma = 100.0f;

        for (int f = 1; f < s.numFrames; ++f)
        {
            const auto* cur = s.frame (f);
            const auto* prev = s.frame (f - 1);

            float sum = 0.0f;

            for (int k = 0; k < s.numBins; ++k)
            {
                const auto d = std::log1p (gamma * cur[k]) - std::log1p (gamma * prev[k]);

                if (d > 0.0f)
                    sum += d;
            }

            flux[(size_t) f] = sum;
        }

        const auto peak = flux.empty() ? 0.0f : *std::max_element (flux.begin(), flux.end());

        if (peak > 0.0f)
            for (auto& v : flux)
                v /= peak;

        return flux;
    }

    /** Peak-picks the flux against a sliding median floor.  A fixed threshold
        fails the moment the arrangement gets denser; the median tracks it.     */
    std::vector<int> pickOnsetFrames (const std::vector<float>& flux, double sensitivity, double frameRate)
    {
        std::vector<int> onsets;

        const auto n = (int) flux.size();

        if (n < 3)
            return onsets;

        const auto sens    = juce::jlimit (0.0, 1.0, sensitivity);
        const auto half    = juce::jmax (3, (int) std::round (frameRate * 0.08));   // +-80 ms
        const auto lambda  = (float) (1.0 + (1.0 - sens) * 1.6);   // 1.0 (permissive) .. 2.6 (strict)
        const auto delta   = (float) (0.015 + (1.0 - sens) * 0.055);
        const auto minGap  = juce::jmax (1, (int) std::round (frameRate * 0.03));   // 30 ms refractory

        std::vector<float> window;
        window.reserve ((size_t) (2 * half + 1));

        int lastOnset = -minGap - 1;

        for (int f = 1; f < n - 1; ++f)
        {
            // local maximum first - it is the cheap test
            if (flux[(size_t) f] <= flux[(size_t) (f - 1)] || flux[(size_t) f] < flux[(size_t) (f + 1)])
                continue;

            if (f - lastOnset < minGap)
                continue;

            window.assign (flux.begin() + juce::jmax (0, f - half),
                           flux.begin() + juce::jmin (n, f + half + 1));

            const auto median = medianOf (window);

            if (flux[(size_t) f] > lambda * median + delta)
            {
                onsets.push_back (f);
                lastOnset = f;
            }
        }

        return onsets;
    }

    //==========================================================================
    //  YIN pitch tracking (pYIN-style, minus the HMM)
    //==========================================================================
    struct PitchFrame
    {
        float midi       = 0.0f;
        float confidence = 0.0f;   // 1 - aperiodicity
        float rms        = 0.0f;
    };

    /** Fast YIN: the difference function is computed from an FFT
        cross-correlation plus a running sum of squares, which turns the naive
        O(W * tauMax) inner loop into three FFTs per frame.

        ponytail: one candidate per frame, no pYIN HMM - octave slips inside a
        note are corrected by the median in segmentation rather than by a Viterbi
        decode over multiple candidates.  Add the HMM if octave errors survive
        into real transcriptions.                                               */
    class YinTracker
    {
    public:
        YinTracker (const std::vector<float>& signal, double sr)
            : x (signal), sampleRate (sr), fft (kYinFftOrder)
        {
            cumulativeSquares.resize (x.size() + 1, 0.0);

            for (size_t i = 0; i < x.size(); ++i)
                cumulativeSquares[i + 1] = cumulativeSquares[i] + (double) x[i] * (double) x[i];

            fa.resize   ((size_t) kYinFftSize);
            fb.resize   ((size_t) kYinFftSize);
            spec.resize ((size_t) kYinFftSize);
            corr.resize ((size_t) kYinFftSize);
            diff.resize ((size_t) kYinWindow);
            cmnd.resize ((size_t) kYinWindow);
        }

        /** Cheap enough to call before deciding whether a frame is worth analysing. */
        float rmsAt (int start) const noexcept
        {
            return (float) std::sqrt (energy (start, kYinWindow) / (double) kYinWindow);
        }

        int requiredSamples() const noexcept { return 2 * kYinWindow; }

        PitchFrame analyse (int start, double minHz, double maxHz)
        {
            PitchFrame out;

            if (start < 0 || (size_t) start + (size_t) requiredSamples() > x.size())
                return out;

            const auto power0 = energy (start, kYinWindow);
            out.rms = (float) std::sqrt (power0 / (double) kYinWindow);

            if (power0 <= 1.0e-12)
                return out;

            // fa = reference window zero-padded, fb = twice its length, so the
            // circular correlation is linear for every lag we look at.
            for (int i = 0; i < kYinFftSize; ++i)
            {
                const auto sample = x[(size_t) (start + i)];
                fa[(size_t) i] = { i < kYinWindow ? sample : 0.0f, 0.0f };
                fb[(size_t) i] = { sample, 0.0f };
            }

            fft.perform (fa.data(), spec.data(), false);
            fft.perform (fb.data(), corr.data(), false);

            for (int k = 0; k < kYinFftSize; ++k)
                spec[(size_t) k] = std::conj (spec[(size_t) k]) * corr[(size_t) k];

            fft.perform (spec.data(), corr.data(), true);

            // JUCE's inverse transform scaling differs between FFT back ends, so
            // calibrate against r(0), which we know exactly from the power sum.
            const auto raw0 = (double) corr[0].real();

            if (std::abs (raw0) < 1.0e-20)
                return out;

            const auto scale = power0 / raw0;

            for (int tau = 0; tau < kYinWindow; ++tau)
            {
                const auto powerTau = energy (start + tau, kYinWindow);
                const auto d = power0 + powerTau - 2.0 * scale * (double) corr[(size_t) tau].real();
                diff[(size_t) tau] = juce::jmax (0.0, d);
            }

            // Cumulative mean normalised difference - this is the step that
            // stops YIN from always choosing tau = 0.
            cmnd[0] = 1.0;
            double running = 0.0;

            for (int tau = 1; tau < kYinWindow; ++tau)
            {
                running += diff[(size_t) tau];
                cmnd[(size_t) tau] = running > 1.0e-20
                                   ? diff[(size_t) tau] * (double) tau / running
                                   : 1.0;
            }

            const auto tauMin = juce::jlimit (2, kYinWindow - 3,
                                              (int) std::ceil (sampleRate / juce::jmax (1.0, maxHz)));
            const auto tauMax = juce::jlimit (tauMin + 1, kYinWindow - 2,
                                              (int) std::floor (sampleRate / juce::jmax (1.0, minHz)));

            int best = -1;

            for (int tau = tauMin; tau <= tauMax; ++tau)
            {
                if (cmnd[(size_t) tau] < kYinThreshold)
                {
                    // walk down to the bottom of this dip rather than taking the
                    // first sample under the threshold
                    while (tau + 1 <= tauMax && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
                        ++tau;

                    best = tau;
                    break;
                }
            }

            if (best < 0)
                best = (int) std::distance (cmnd.begin(),
                                            std::min_element (cmnd.begin() + tauMin,
                                                              cmnd.begin() + tauMax + 1));

            // Parabolic interpolation on the minimum: without it the pitch is
            // quantised to the sample rate and lands sharp on high notes.
            double refined = (double) best;

            if (best > 0 && best < kYinWindow - 1)
            {
                const auto a = cmnd[(size_t) (best - 1)];
                const auto b = cmnd[(size_t) best];
                const auto c = cmnd[(size_t) (best + 1)];
                const auto denom = a - 2.0 * b + c;

                if (std::abs (denom) > 1.0e-12)
                    refined += juce::jlimit (-1.0, 1.0, 0.5 * (a - c) / denom);
            }

            if (refined <= 1.0)
                return out;

            const auto aperiodicity = juce::jlimit (0.0, 1.0, cmnd[(size_t) best]);

            out.midi       = (float) hzToMidi (sampleRate / refined);
            out.confidence = (float) (1.0 - aperiodicity);
            return out;
        }

    private:
        double energy (int start, int length) const noexcept
        {
            const auto lo = (size_t) juce::jlimit (0, (int) x.size(), start);
            const auto hi = (size_t) juce::jlimit (0, (int) x.size(), start + length);
            return cumulativeSquares[hi] - cumulativeSquares[lo];
        }

        const std::vector<float>& x;
        double sampleRate;
        juce::dsp::FFT fft;

        std::vector<double> cumulativeSquares, diff, cmnd;
        std::vector<juce::dsp::Complex<float>> fa, fb, spec, corr;
    };

    //==========================================================================
    //  Polyphonic salience (harmonic sum with partial suppression)
    //==========================================================================

    /** Precomputed bin ranges for every candidate pitch's partials.  Built once
        per spectrogram; recomputing them per frame is where a naive harmonic-sum
        implementation spends all of its time.

        ponytail: a linear-frequency STFT gives ~21 Hz bins, so below about C3 two
        adjacent semitones share a bin and the salience is guesswork; a CQT or a
        second longer-window STFT for the low register is the upgrade.           */
    struct HarmonicMap
    {
        struct Partial { int lo = 0, hi = -1; float weight = 0.0f; };

        int lowPitch = 0, highPitch = -1;
        std::vector<Partial> partials;   // (highPitch - lowPitch + 1) * kNumHarmonics

        const Partial* forPitch (int p) const noexcept
        {
            return partials.data() + (size_t) (p - lowPitch) * (size_t) kNumHarmonics;
        }

        static HarmonicMap build (const Spectrogram& s, int lo, int hi)
        {
            HarmonicMap m;
            m.lowPitch  = lo;
            m.highPitch = juce::jmax (lo, hi);
            m.partials.resize ((size_t) (m.highPitch - m.lowPitch + 1) * (size_t) kNumHarmonics);

            const auto maxHz = s.binHz * (s.numBins - 1);

            for (int p = m.lowPitch; p <= m.highPitch; ++p)
            {
                const auto f0 = midiToHz (p);

                for (int h = 1; h <= kNumHarmonics; ++h)
                {
                    auto& partial = m.partials[(size_t) (p - m.lowPitch) * (size_t) kNumHarmonics
                                               + (size_t) (h - 1)];

                    const auto hz = f0 * h;

                    if (hz <= 0.0 || hz >= maxHz)
                        continue;                       // stays lo > hi == "skip"

                    const auto centre = (int) std::round (hz / s.binHz);
                    // ~half a semitone either side, at least one bin
                    const auto width  = juce::jmax (1, (int) std::round (hz * 0.029 / s.binHz));

                    partial.lo     = juce::jmax (0, centre - width);
                    partial.hi     = juce::jmin (s.numBins - 1, centre + width);
                    partial.weight = 1.0f / (float) h;  // 1/h rolloff matches natural spectra
                }
            }

            return m;
        }
    };

    /** Iterative harmonic-sum peak picking for one frame.  Picks the strongest
        pitch, attenuates its partials, repeats.  Attenuation rather than
        subtraction, because partials genuinely shared with a second voice would
        otherwise take that voice down with them.                               */
    void saliencePeaks (const Spectrogram& s, int frameIndex, const HarmonicMap& map,
                        int maxVoices, std::vector<float>& scratch,
                        std::vector<std::pair<int, float>>& out)
    {
        out.clear();

        const auto* frame = s.frame (frameIndex);
        scratch.assign (frame, frame + s.numBins);

        const auto salienceFor = [&] (int pitch)
        {
            const auto* partials = map.forPitch (pitch);
            float sum = 0.0f;

            for (int h = 0; h < kNumHarmonics; ++h)
            {
                const auto& partial = partials[h];
                float peak = 0.0f;

                for (int k = partial.lo; k <= partial.hi; ++k)
                    peak = juce::jmax (peak, scratch[(size_t) k]);

                sum += peak * partial.weight;
            }

            return sum;
        };

        float firstPeak = 0.0f;

        for (int v = 0; v < maxVoices; ++v)
        {
            int   bestPitch = -1;
            float bestSalience = 0.0f;

            for (int p = map.lowPitch; p <= map.highPitch; ++p)
            {
                const auto sal = salienceFor (p);

                if (sal > bestSalience)
                {
                    bestSalience = sal;
                    bestPitch    = p;
                }
            }

            if (bestPitch < 0 || bestSalience <= 0.0f)
                break;

            if (v == 0)
                firstPeak = bestSalience;
            else if (bestSalience < 0.22f * firstPeak)
                break;

            out.emplace_back (bestPitch, bestSalience);

            const auto* partials = map.forPitch (bestPitch);

            for (int h = 0; h < kNumHarmonics; ++h)
                for (int k = partials[h].lo; k <= partials[h].hi; ++k)
                    scratch[(size_t) k] *= 0.15f;
        }
    }

    //==========================================================================
    //  Tempo and swing
    //==========================================================================

    /** Autocorrelation of the onset envelope with a log-normal tempo prior
        around 120 BPM, which is what keeps it off the half/double-time lag.    */
    double estimateBpm (const std::vector<float>& envelope, double frameRate)
    {
        const auto n = (int) envelope.size();

        if (n < 64 || frameRate <= 0.0)
            return 0.0;

        double mean = 0.0;
        for (auto v : envelope) mean += v;
        mean /= (double) n;

        std::vector<double> centred ((size_t) n);
        for (int i = 0; i < n; ++i) centred[(size_t) i] = envelope[(size_t) i] - mean;

        const auto minLag = juce::jmax (2, (int) std::floor (60.0 * frameRate / 220.0));
        const auto maxLag = juce::jmin (n / 2, (int) std::ceil (60.0 * frameRate / 40.0));

        if (maxLag <= minLag)
            return 0.0;

        double bestScore = 0.0, bestBpm = 0.0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double acc = 0.0;

            for (int i = 0; i + lag < n; ++i)
                acc += centred[(size_t) i] * centred[(size_t) (i + lag)];

            acc /= (double) (n - lag);

            const auto bpm    = 60.0 * frameRate / (double) lag;
            const auto octave = std::log2 (bpm / 120.0) / 0.9;
            const auto prior  = std::exp (-0.5 * octave * octave);
            const auto score  = acc * prior;

            if (score > bestScore)
            {
                bestScore = score;
                bestBpm   = bpm;
            }
        }

        return bestBpm;
    }

    /** Swing from the distribution of eighth-note pairs: for every pair of
        consecutive inter-onset intervals that together make a quarter note, the
        first one's share of the pair is the swing ratio.  0.5 == straight,
        0.667 == triplet swing.                                                 */
    double estimateSwing (const std::vector<double>& onsetSeconds, double bpm)
    {
        if (bpm <= 0.0 || onsetSeconds.size() < 6)
            return 0.5;

        const auto quarter = 60.0 / bpm;

        std::vector<double> ratios;

        for (size_t i = 0; i + 2 < onsetSeconds.size(); ++i)
        {
            const auto a = onsetSeconds[i + 1] - onsetSeconds[i];
            const auto b = onsetSeconds[i + 2] - onsetSeconds[i + 1];

            if (a <= 0.0 || b <= 0.0)
                continue;

            const auto pair = a + b;

            if (std::abs (pair - quarter) > quarter * 0.18)
                continue;                                   // not an eighth-note pair

            ratios.push_back (a / pair);
        }

        if (ratios.size() < 3)
            return 0.5;

        // Median, not mean: a couple of misfired onsets would drag a mean.
        const auto med = medianOf (ratios);
        return juce::jlimit (0.5, 0.75, med);
    }

    /** ss::applyQuantise, plus the swung grid when we detected one.  A swung
        eighth grid is not a uniform grid, so it cannot go through applyQuantise
        directly - the off-beat target moves.                                   */
    double snapWithSwing (double beats, Quantise q, double strength, double swing)
    {
        if (swing <= 0.505 || (q != Quantise::eighth && q != Quantise::sixteenth))
            return applyQuantise (beats, q, strength);

        const auto step = quantiseStepInBeats (q);

        if (step <= 0.0 || strength <= 0.0)
            return beats;

        const auto pairLength = step * 2.0;
        const auto pairIndex  = std::floor (beats / pairLength);
        const auto within     = beats - pairIndex * pairLength;

        const double targets[] { 0.0, swing * pairLength, pairLength };

        double bestTarget = 0.0, bestDistance = 1.0e18;

        for (auto t : targets)
        {
            const auto d = std::abs (within - t);

            if (d < bestDistance)
            {
                bestDistance = d;
                bestTarget   = t;
            }
        }

        const auto snapped = pairIndex * pairLength + bestTarget;
        return beats + (snapped - beats) * juce::jlimit (0.0, 1.0, strength);
    }

    //==========================================================================
    //  Shared helpers
    //==========================================================================

    int velocityFromLevel (float level, float reference)
    {
        const auto rel = juce::jlimit (1.0e-4f, 1.0f, level / juce::jmax (1.0e-6f, reference));
        const auto db  = juce::jlimit (-42.0f, 0.0f, juce::Decibels::gainToDecibels (rel));
        return juce::jlimit (1, 127, juce::roundToInt (juce::jmap (db, -42.0f, 0.0f, 28.0f, 127.0f)));
    }
}

//==============================================================================
//  Impl
//==============================================================================
struct Transcriber::Impl
{
    // Kept from the last run so suggestFixes() can offer answers that come out
    // of the analysis rather than out of a table of generic guesses.
    std::vector<double> onsetBeats;
    std::vector<float>  onsetStrength;   // parallel to onsetBeats
    theory::Key         key;
    double              bpm = 120.0;
    Options             lastOptions;
};

Transcriber::Transcriber() : impl (std::make_unique<Impl>()) {}
Transcriber::~Transcriber() = default;

//==============================================================================
Transcriber::Result Transcriber::transcribe (const juce::AudioBuffer<float>& audio, double sampleRate,
                                             const Options& options, ProgressFn progress)
{
    Result result;

    // False once the caller has asked to stop, and false for ever after - the
    // callback is not asked again, so every checkpoint downstream agrees.
    bool aborted = false;

    const auto report = [&progress, &aborted] (float p)
    {
        if (! aborted && progress && ! progress (juce::jlimit (0.0f, 1.0f, p)))
            aborted = true;

        return ! aborted;
    };

    // Bailing out has to leave the caller with something it can recognise as a
    // cancellation rather than as an empty transcription.
    const auto giveUp = [&result] ()
    {
        result.cancelled = true;
        result.notes.clear();
        result.message   = "Cancelled.";
        return result;
    };

    if (! report (0.0f))
        return giveUp();

    if (sampleRate < 8000.0 || audio.getNumSamples() < 4 * kYinWindow || audio.getNumChannels() < 1)
    {
        result.message = "Audio is too short to analyse.";
        report (1.0f);
        return result;
    }

    //--- mono downmix ---------------------------------------------------------
    const auto numSamples = audio.getNumSamples();
    std::vector<float> mono ((size_t) numSamples, 0.0f);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* src = audio.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += src[i];
    }

    {
        const auto invChannels = 1.0f / (float) audio.getNumChannels();
        double sum = 0.0;

        for (auto& v : mono) { v *= invChannels; sum += v; }

        // DC offset wrecks YIN's difference function, and cheap interfaces have it.
        const auto dc = (float) (sum / (double) numSamples);

        if (std::abs (dc) > 1.0e-5f)
            for (auto& v : mono)
                v -= dc;
    }

    const auto hop = juce::jmax (1, (int) std::round (sampleRate * kHopSeconds));
    const auto secondsPerFrame = (double) hop / sampleRate;

    if (! report (0.05f))
        return giveUp();

    //--- STFT -----------------------------------------------------------------
    // ponytail: the whole spectrogram is held in RAM (~0.4 MB per second of
    // audio) and computed in one uninterruptible call, so a cancel during it
    // waits for the transform (seconds, not minutes - the pitch loops below are
    // where the time actually goes).  Streaming it frame by frame would fix both
    // the memory and the cancel latency at once.
    const auto spec = computeStft (mono, sampleRate, hop);

    if (spec.numFrames < 4)
    {
        result.message = "Audio is too short to analyse.";
        report (1.0f);
        return result;
    }

    if (! report (0.2f))
        return giveUp();

    //--- onsets ---------------------------------------------------------------
    const auto flux        = spectralFlux (spec);
    const auto onsetFrames = pickOnsetFrames (flux, options.sensitivity, spec.frameRate);

    std::vector<char> onsetFlags ((size_t) spec.numFrames, 0);
    std::vector<double> onsetSeconds;
    onsetSeconds.reserve (onsetFrames.size());

    for (auto f : onsetFrames)
    {
        onsetFlags[(size_t) f] = 1;
        onsetSeconds.push_back (juce::jmax (0.0, (double) f * secondsPerFrame - kOnsetBackoffSeconds));
    }

    if (! report (0.3f))
        return giveUp();

    //--- tempo / swing --------------------------------------------------------
    auto bpm = options.detectTempo ? estimateBpm (flux, spec.frameRate) : 0.0;

    if (bpm < 40.0 || bpm > 220.0)
        bpm = 120.0;                     // nothing periodic enough to trust

    const auto swing = options.detectSwing ? estimateSwing (onsetSeconds, bpm) : 0.5;
    const auto beatsPerSecond = bpm / 60.0;

    result.estimatedBpm = bpm;
    result.swingRatio   = swing;

    //--- mode -----------------------------------------------------------------
    const auto lowPitch  = juce::jlimit (24, 100, juce::jmax (options.minPitch, 33));
    const auto highPitch = juce::jlimit (lowPitch + 1, 108, juce::jmin (options.maxPitch, 96));
    const auto harmonics = HarmonicMap::build (spec, lowPitch, highPitch);

    auto mode = options.mode;

    if (mode == Mode::automatic)
    {
        // Sample the loud frames and count how many salience peaks survive.  We
        // never auto-select drums: percussive piano would trip it constantly,
        // and getting that wrong costs the user the whole transcription.
        std::vector<float> scratch;
        std::vector<std::pair<int, float>> peaks;

        const auto stride = juce::jmax (1, spec.numFrames / 200);
        double peakSum = 0.0;
        int    sampled = 0;

        for (int f = 0; f < spec.numFrames; f += stride)
        {
            if (flux[(size_t) f] < 0.02f && f > 0)
                continue;

            saliencePeaks (spec, f, harmonics, kMaxPolyVoices, scratch, peaks);

            if (peaks.empty())
                continue;

            const auto strongest = peaks.front().second;
            int significant = 0;

            for (const auto& p : peaks)
                if (p.second >= 0.35f * strongest)
                    ++significant;

            peakSum += significant;
            ++sampled;
        }

        const auto meanPeaks = sampled > 0 ? peakSum / (double) sampled : 1.0;
        mode = meanPeaks > 1.7 ? Mode::polyphonic : Mode::monophonic;
    }

    if (! report (0.35f))
        return giveUp();

    //--- pitch / notes --------------------------------------------------------
    std::vector<Note> notes;

    if (mode == Mode::drums)
    {
        //----------------------------------------------------------------------
        //  Drums: classify each onset by where its energy sits.
        //----------------------------------------------------------------------
        const auto binOf = [&spec] (double hz)
        {
            return juce::jlimit (0, spec.numBins - 1, (int) std::round (hz / spec.binHz));
        };

        const auto lowEnd  = binOf (150.0);
        const auto midEnd  = binOf (1500.0);
        const auto highLo  = binOf (4000.0);
        const auto highEnd = binOf (12000.0);

        float loudest = 1.0e-6f;

        for (auto f : onsetFrames)
            loudest = juce::jmax (loudest, flux[(size_t) f]);

        for (size_t i = 0; i < onsetFrames.size(); ++i)
        {
            if ((i & 63) == 0
                 && ! report (0.35f + 0.5f * (float) i / (float) juce::jmax ((size_t) 1, onsetFrames.size())))
                return giveUp();

            const auto f = onsetFrames[i];
            const auto* frame = spec.frame (f);

            double low = 0.0, mid = 0.0, high = 0.0, weighted = 0.0, total = 0.0;

            for (int k = 0; k < spec.numBins; ++k)
            {
                const auto m = (double) frame[k];

                total    += m;
                weighted += m * (double) k * spec.binHz;

                if (k <= lowEnd)                        low  += m;
                else if (k <= midEnd)                   mid  += m;
                else if (k >= highLo && k <= highEnd)   high += m;
            }

            if (total <= 1.0e-9)
                continue;

            const auto centroid = weighted / total;
            const auto banded   = juce::jmax (1.0e-9, low + mid + high);

            int   pitch = 38;                       // acoustic snare
            double dominance = mid / banded;

            if (low / banded > 0.55 && centroid < 500.0)
            {
                pitch     = 36;                     // bass drum
                dominance = low / banded;
            }
            else if (high / banded > 0.42 && centroid > 2500.0)
            {
                // Open vs closed from how long the energy hangs around: a closed
                // hat is gone inside ~60 ms, an open one is not.
                const auto tailFrames = juce::jmin (spec.numFrames - f - 1,
                                                    (int) std::round (0.12 / secondsPerFrame));
                double tail = 0.0;

                for (int t = 1; t <= tailFrames; ++t)
                {
                    const auto* later = spec.frame (f + t);

                    for (int k = highLo; k <= highEnd; ++k)
                        tail += (double) later[k];
                }

                const auto sustain = tailFrames > 0 ? tail / ((double) tailFrames * juce::jmax (1.0e-9, high)) : 0.0;

                pitch     = sustain > 0.55 ? 46 : 42;   // open / closed hi-hat
                dominance = high / banded;
            }

            Note n;
            n.pitch       = pitch;
            n.startBeats  = juce::jmax (0.0, ((double) f * secondsPerFrame - kOnsetBackoffSeconds)) * beatsPerSecond;
            n.lengthBeats = 0.25;                       // one-shots; length is cosmetic
            n.velocity    = velocityFromLevel (flux[(size_t) f], loudest);
            n.channel     = 10;                         // GM drum channel
            // Dominance is the honest number here: an ambiguous hit that could be
            // a snare or a tom really is a coin flip, and the UI should show that.
            n.confidence  = (float) juce::jlimit (0.30, 0.92, (dominance - 0.33) / 0.45 * 0.62 + 0.30);

            notes.push_back (n);
        }

        result.message = "Drum mode: hits are mapped to GM kick/snare/hi-hat only. "
                         "Toms, cymbals and ghost-note dynamics are not detected.";

        if (! report (0.85f))
            return giveUp();
    }
    else if (mode == Mode::polyphonic)
    {
        //----------------------------------------------------------------------
        //  Polyphonic: harmonic-sum salience -> per-pitch activation runs.
        //----------------------------------------------------------------------
        struct Run
        {
            int    startFrame   = -1;
            int    lastFrame    = -1;
            double salienceSum  = 0.0;
            int    count        = 0;
            bool   startedOnOnset = false;
        };

        std::array<Run, 128> open {};
        std::vector<float> scratch;
        std::vector<std::pair<int, float>> peaks;

        const auto minFrames  = juce::jmax (1, (int) std::round (options.minNoteLengthMs * 0.001 * spec.frameRate));
        const auto gapFrames  = 2;
        const auto activation = 0.30f;

        const auto closeRun = [&] (int pitch, Run& run)
        {
            if (run.startFrame >= 0 && run.count >= minFrames)
            {
                const auto meanSalience = (float) (run.salienceSum / (double) run.count);

                Note n;
                n.pitch       = pitch;
                n.startBeats  = (double) run.startFrame * secondsPerFrame * beatsPerSecond;
                n.lengthBeats = juce::jmax (1, run.lastFrame - run.startFrame + 1) * secondsPerFrame * beatsPerSecond;
                n.velocity    = juce::jlimit (1, 127, juce::roundToInt (juce::jmap (juce::jlimit (0.0f, 1.0f, meanSalience),
                                                                                    0.0f, 1.0f, 45.0f, 112.0f)));
                // Deliberately capped well below the monophonic path: spec 15.2.
                n.confidence  = juce::jlimit (0.0f, 1.0f, meanSalience * kPolyReliability
                                                          * (run.startedOnOnset ? 1.0f : 0.8f));
                notes.push_back (n);
            }

            run = {};
        };

        for (int f = 0; f < spec.numFrames; ++f)
        {
            saliencePeaks (spec, f, harmonics, kMaxPolyVoices, scratch, peaks);

            std::array<float, 128> active {};

            if (! peaks.empty())
            {
                const auto strongest = juce::jmax (1.0e-9f, peaks.front().second);

                for (const auto& p : peaks)
                {
                    const auto normalised = p.second / strongest;

                    if (normalised >= activation)
                        active[(size_t) juce::jlimit (0, 127, p.first)] = normalised;
                }
            }

            for (int pitch = lowPitch; pitch <= highPitch; ++pitch)
            {
                auto& run = open[(size_t) pitch];
                const auto strength = active[(size_t) pitch];

                if (strength > 0.0f)
                {
                    if (run.startFrame < 0)
                    {
                        run.startFrame     = f;
                        run.startedOnOnset = onsetFlags[(size_t) f] != 0;
                    }
                    else if (onsetFlags[(size_t) f] != 0 && f - run.startFrame >= minFrames)
                    {
                        closeRun (pitch, run);
                        run.startFrame     = f;
                        run.startedOnOnset = true;
                    }

                    run.lastFrame    = f;
                    run.salienceSum += strength;
                    ++run.count;
                }
                else if (run.startFrame >= 0 && f - run.lastFrame > gapFrames)
                {
                    closeRun (pitch, run);
                }
            }

            if ((f & 63) == 0 && ! report (0.35f + 0.5f * (float) f / (float) spec.numFrames))
                return giveUp();
        }

        for (int pitch = lowPitch; pitch <= highPitch; ++pitch)
            closeRun (pitch, open[(size_t) pitch]);

        result.message = "Polyphonic mode: overlapping harmonics limit accuracy (spec 15.2). "
                         "Confidence is scored conservatively - review the pale notes first.";
    }
    else
    {
        //----------------------------------------------------------------------
        //  Monophonic: YIN per frame, then group frames into notes.
        //----------------------------------------------------------------------
        YinTracker yin (mono, sampleRate);

        const auto numPitchFrames = (int) (((juce::int64) numSamples - yin.requiredSamples()) / hop) + 1;

        if (numPitchFrames < 2)
        {
            result.message = "Audio is too short to analyse.";
            report (1.0f);
            return result;
        }

        const auto minHz = midiToHz (juce::jmax (12, options.minPitch));
        const auto maxHz = juce::jmin (sampleRate * 0.45, midiToHz (juce::jmin (127, options.maxPitch)));

        std::vector<PitchFrame> frames ((size_t) numPitchFrames);
        float peakRms = 1.0e-6f;

        for (int f = 0; f < numPitchFrames; ++f)
        {
            const auto start = f * hop;

            // Skip the silence outright - YIN on a noise floor is three FFTs
            // spent producing a number we would throw away anyway.
            const auto level = yin.rmsAt (start);

            if (level < kSilenceRms)
            {
                frames[(size_t) f].rms = level;
                continue;
            }

            frames[(size_t) f] = yin.analyse (start, minHz, maxHz);
            peakRms = juce::jmax (peakRms, frames[(size_t) f].rms);

            if ((f & 63) == 0 && ! report (0.35f + 0.5f * (float) f / (float) numPitchFrames))
                return giveUp();
        }

        //--- segmentation ------------------------------------------------------
        struct Building
        {
            std::vector<float> pitches, confidences, levels;
            int  startFrame = -1;
            bool startedOnOnset = false;
            float reference = 0.0f;      // running median pitch of this note
        };

        Building current;

        const auto minFrames = juce::jmax (1, (int) std::round (options.minNoteLengthMs * 0.001 * spec.frameRate));
        const auto voicedThreshold = 0.30f + 0.20f * (float) (1.0 - juce::jlimit (0.0, 1.0, options.sensitivity));

        const auto flush = [&] ()
        {
            if (current.startFrame >= 0 && (int) current.pitches.size() >= minFrames)
            {
                const auto medianPitch = medianOf (current.pitches);
                const auto medianConf  = medianOf (current.confidences);

                // How far the frame pitches wandered.  A note the tracker held
                // rock steady deserves more trust than one it argued with.
                double deviation = 0.0;

                for (auto p : current.pitches)
                    deviation += std::abs ((double) p - (double) medianPitch);

                deviation /= (double) current.pitches.size();

                const auto stability = (float) juce::jlimit (0.5, 1.0,
                                            1.0 - deviation / juce::jmax (0.25, options.pitchBendTolerance));
                const auto onsetFactor = current.startedOnOnset ? 1.0f : 0.8f;

                const auto level = *std::max_element (current.levels.begin(), current.levels.end());

                Note n;
                n.pitch       = juce::jlimit (options.minPitch, options.maxPitch, juce::roundToInt (medianPitch));
                n.startBeats  = (double) current.startFrame * secondsPerFrame * beatsPerSecond;
                n.lengthBeats = (double) current.pitches.size() * secondsPerFrame * beatsPerSecond;
                n.velocity    = velocityFromLevel (level, peakRms);
                n.confidence  = juce::jlimit (0.0f, 1.0f, medianConf * onsetFactor * stability);

                notes.push_back (n);
            }

            current = {};
        };

        for (int f = 0; f < numPitchFrames; ++f)
        {
            const auto& frame = frames[(size_t) f];

            const auto voiced = frame.confidence >= voicedThreshold
                             && frame.rms >= kSilenceRms
                             && frame.midi >= (float) options.minPitch - 0.5f
                             && frame.midi <= (float) options.maxPitch + 0.5f;

            if (! voiced)
            {
                flush();
                continue;
            }

            const auto onsetHere = f < (int) onsetFlags.size() && onsetFlags[(size_t) f] != 0;

            if (current.startFrame < 0)
            {
                current.startFrame     = f;
                current.startedOnOnset = onsetHere;
            }
            else if (onsetHere
                     || std::abs (frame.midi - current.reference) > (float) options.pitchBendTolerance)
            {
                flush();
                current.startFrame     = f;
                current.startedOnOnset = onsetHere;
            }

            current.pitches.push_back (frame.midi);
            current.confidences.push_back (frame.confidence);
            current.levels.push_back (frame.rms);

            // The reference pitch freezes once the note is established: a slow
            // glide should trip pitchBendTolerance, not drag the target with it.
            if (current.pitches.size() <= 8)
                current.reference = medianOf (current.pitches);
        }

        flush();

        result.message = notes.empty() ? "No pitched material found - try raising the sensitivity."
                                       : juce::String();
    }

    if (! report (0.9f))
        return giveUp();

    //--- clean-up, quantise, key ---------------------------------------------
    removeBelowConfidence (notes, options.confidenceFloor);

    std::sort (notes.begin(), notes.end(),
               [] (const Note& a, const Note& b)
               {
                   return a.startBeats != b.startBeats ? a.startBeats < b.startBeats : a.pitch < b.pitch;
               });

    if (options.quantise != Quantise::off && options.quantiseStrength > 0.0)
    {
        const auto step = quantiseStepInBeats (options.quantise);

        for (auto& n : notes)
        {
            const auto end = snapWithSwing (n.endBeats(), options.quantise, options.quantiseStrength, swing);
            n.startBeats = juce::jmax (0.0, snapWithSwing (n.startBeats, options.quantise,
                                                           options.quantiseStrength, swing));
            n.lengthBeats = juce::jmax (step * 0.5, end - n.startBeats);
        }
    }

    if (options.detectKey)
        result.key = theory::estimateKey (notes);

    float confidenceSum = 0.0f;

    for (const auto& n : notes)
        confidenceSum += n.confidence;

    result.notes          = std::move (notes);
    result.meanConfidence = result.notes.empty() ? 0.0f
                                                 : confidenceSum / (float) result.notes.size();

    //--- remember the analysis for suggestFixes() -----------------------------
    impl->onsetBeats.clear();
    impl->onsetStrength.clear();
    impl->onsetBeats.reserve (onsetFrames.size());
    impl->onsetStrength.reserve (onsetFrames.size());

    for (size_t i = 0; i < onsetFrames.size(); ++i)
    {
        impl->onsetBeats.push_back (onsetSeconds[i] * beatsPerSecond);
        impl->onsetStrength.push_back (flux[(size_t) onsetFrames[i]]);
    }

    impl->key         = result.key;
    impl->bpm         = bpm;
    impl->lastOptions = options;

    report (1.0f);
    return result;
}

//==============================================================================
Transcriber::Result Transcriber::transcribeFile (const juce::File& file, juce::AudioFormatManager& formats,
                                                 const Options& options, ProgressFn progress)
{
    Result result;

    const auto report = [&progress] (float p) { return progress == nullptr || progress (p); };

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        result.message = "Could not read " + file.getFileName() + " - unsupported or missing file.";
        report (1.0f);
        return result;
    }

    // ponytail: the file is decoded into RAM in one go and capped at 15 minutes;
    // block-wise analysis would lift the cap if long-form material ever matters.
    const auto maxSamples = (juce::int64) (reader->sampleRate * 15.0 * 60.0);
    const auto wanted     = juce::jmin (reader->lengthInSamples, maxSamples);

    if (wanted <= 0 || reader->numChannels == 0)
    {
        result.message = "The file contains no audio.";
        report (1.0f);
        return result;
    }

    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) wanted);
    reader->read (&buffer, 0, (int) wanted, 0, true, true);

    const auto truncated = reader->lengthInSamples > wanted;
    const auto sampleRate = reader->sampleRate;

    reader.reset();

    // Reserve the first 2% of the bar for decoding so the caller's progress
    // display does not sit at zero while a long file loads.
    // ponytail: the decode above is one blocking read, so cancel is only felt
    // from here on; read it in blocks with a checkpoint per block to fix that.
    if (! report (0.02f))
    {
        result.cancelled = true;
        result.message   = "Cancelled.";
        return result;
    }

    result = transcribe (buffer, sampleRate, options,
                         [&report] (float p) { return report (0.02f + 0.98f * p); });

    if (result.cancelled)
        return result;

    if (truncated)
        result.message = result.message.isEmpty()
                       ? juce::String ("Only the first 15 minutes were transcribed.")
                       : result.message + " Only the first 15 minutes were transcribed.";

    return result;
}

//==============================================================================
std::vector<Transcriber::Suggestion> Transcriber::suggestFixes (const Result& result, int noteIndex) const
{
    std::vector<Suggestion> out;

    if (noteIndex < 0 || noteIndex >= (int) result.notes.size())
        return out;

    const auto& note = result.notes[(size_t) noteIndex];

    // Context: what register is this phrase actually in?  Octave errors - YIN's
    // signature failure - only look wrong next to their neighbours.
    std::vector<float> neighbours;

    for (int i = juce::jmax (0, noteIndex - 3);
         i <= juce::jmin ((int) result.notes.size() - 1, noteIndex + 3); ++i)
        if (i != noteIndex)
            neighbours.push_back ((float) result.notes[(size_t) i].pitch);

    const auto centre = neighbours.empty() ? (float) note.pitch : medianOf (neighbours);

    const auto melodicFit = [centre] (int pitch)
    {
        // Melodies step far more often than they leap, so distance from the
        // local centre is a usable prior.
        return (float) std::exp (-std::abs ((double) pitch - (double) centre) / 7.0);
    };

    const auto keyFit = [&result] (int pitch)
    {
        return theory::isInScale (pitch, result.key) ? 1.0f : 0.55f;
    };

    const auto doubt      = 1.0f - juce::jlimit (0.0f, 1.0f, note.confidence);
    const auto currentFit = melodicFit (note.pitch) * keyFit (note.pitch);

    const auto addPitchFix = [&] (const juce::String& label, int newPitch)
    {
        if (newPitch < 0 || newPitch > 127 || newPitch == note.pitch)
            return;

        const auto fit = melodicFit (newPitch) * keyFit (newPitch);

        if (fit <= currentFit)
            return;                       // not actually an improvement; do not offer it

        auto fixed = note;
        fixed.pitch      = newPitch;
        fixed.confidence = juce::jlimit (0.0f, 1.0f, note.confidence + 0.5f * (fit - currentFit));

        Suggestion s;
        s.label        = label + " (" + theory::midiNoteName (newPitch) + ")";
        s.firstIndex   = noteIndex;
        s.count        = 1;
        s.replacements = { fixed };
        s.score        = juce::jlimit (0.0f, 1.0f, 0.35f * doubt + 0.65f * (fit - currentFit));

        out.push_back (s);
    };

    addPitchFix ("Octave up",       note.pitch + 12);
    addPitchFix ("Octave down",     note.pitch - 12);
    addPitchFix ("Up a semitone",   note.pitch + 1);
    addPitchFix ("Down a semitone", note.pitch - 1);

    if (! theory::isInScale (note.pitch, result.key))
        addPitchFix ("Snap to " + theory::toString (result.key),
                     theory::snapToScale (note.pitch, result.key));

    // Split: a long, shaky note with an onset inside it is usually two notes
    // that the segmenter glued together.
    const auto minLengthBeats = impl->lastOptions.minNoteLengthMs * 0.001
                              * juce::jmax (1.0, impl->bpm) / 60.0;

    for (size_t i = 0; i < impl->onsetBeats.size(); ++i)
    {
        const auto beat = impl->onsetBeats[i];

        if (beat > note.startBeats + minLengthBeats && beat < note.endBeats() - minLengthBeats)
        {
            // Both halves, not just the first: the onset says a second note
            // starts here, so the suggestion has to be able to say so too.
            auto first  = note;
            auto second = note;

            first.lengthBeats  = beat - note.startBeats;
            second.startBeats  = beat;
            second.lengthBeats = note.endBeats() - beat;

            Suggestion s;
            s.label        = "Split at beat " + juce::String (beat, 2);
            s.firstIndex   = noteIndex;
            s.count        = 1;
            s.replacements = { first, second };
            s.score        = juce::jlimit (0.0f, 1.0f, 0.4f * doubt + 0.6f * impl->onsetStrength[i]);

            out.push_back (s);
            break;
        }
    }

    // Merge: same pitch, near-zero gap - the tracker dropped out for a frame.
    if (noteIndex + 1 < (int) result.notes.size())
    {
        const auto& next = result.notes[(size_t) noteIndex + 1];
        const auto gap = next.startBeats - note.endBeats();

        if (next.pitch == note.pitch && gap < 0.15 && gap > -0.05)
        {
            auto merged = note;
            merged.lengthBeats = next.endBeats() - note.startBeats;
            merged.confidence  = juce::jmax (note.confidence, next.confidence);

            // Two notes in, one out - so applying it actually removes the
            // stray fragment instead of leaving it behind the merged note.
            Suggestion s;
            s.label        = "Merge with the next note";
            s.firstIndex   = noteIndex;
            s.count        = 2;
            s.replacements = { merged };
            s.score        = juce::jlimit (0.0f, 1.0f,
                                           0.5f * doubt
                                           + 0.5f * (float) (1.0 - juce::jlimit (0.0, 0.15, std::abs (gap)) / 0.15));

            out.push_back (s);
        }
    }

    std::sort (out.begin(), out.end(),
               [] (const Suggestion& a, const Suggestion& b) { return a.score > b.score; });

    return out;
}

//==============================================================================
//  Bulk clean-ups (spec 8.2 accuracy panel)
//==============================================================================
void Transcriber::removeBelowConfidence (std::vector<Note>& notes, float threshold)
{
    notes.erase (std::remove_if (notes.begin(), notes.end(),
                                 [threshold] (const Note& n) { return n.confidence < threshold; }),
                 notes.end());
}

void Transcriber::removeShorterThan (std::vector<Note>& notes, double beats)
{
    notes.erase (std::remove_if (notes.begin(), notes.end(),
                                 [beats] (const Note& n) { return n.lengthBeats < beats; }),
                 notes.end());
}

void Transcriber::mergeRepeatedPitches (std::vector<Note>& notes, double gapBeats)
{
    if (notes.size() < 2)
        return;

    // Group by pitch so the merge is a single linear pass per voice.
    std::sort (notes.begin(), notes.end(),
               [] (const Note& a, const Note& b)
               {
                   return a.pitch != b.pitch ? a.pitch < b.pitch : a.startBeats < b.startBeats;
               });

    std::vector<Note> merged;
    merged.reserve (notes.size());

    for (const auto& n : notes)
    {
        if (! merged.empty())
        {
            auto& last = merged.back();

            if (last.pitch == n.pitch && n.startBeats - last.endBeats() <= gapBeats)
            {
                last.lengthBeats = juce::jmax (last.endBeats(), n.endBeats()) - last.startBeats;
                last.velocity    = juce::jmax (last.velocity, n.velocity);
                last.confidence  = juce::jmax (last.confidence, n.confidence);
                continue;
            }
        }

        merged.push_back (n);
    }

    std::sort (merged.begin(), merged.end(),
               [] (const Note& a, const Note& b)
               {
                   return a.startBeats != b.startBeats ? a.startBeats < b.startBeats : a.pitch < b.pitch;
               });

    notes = std::move (merged);
}

void Transcriber::snapAllToScale (std::vector<Note>& notes, const theory::Key& key)
{
    for (auto& n : notes)
        n.pitch = theory::snapToScale (n.pitch, key);
}

} // namespace ss
