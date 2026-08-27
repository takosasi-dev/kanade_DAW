#include "Mixer/BuiltinFx.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace ss
{

namespace
{
    constexpr int maxFxChannels = 8;

    using Coeffs = juce::dsp::IIR::Coefficients<float>;
    using Iir    = juce::dsp::IIR::Filter<float>;

    /** A biquad-shaped Coefficients object we can then rewrite in place. */
    Coeffs::Ptr makeBiquadHolder()
    {
        return Coeffs::Ptr (new Coeffs (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f));
    }

    /*  RBJ cookbook coefficients written straight into an existing Coefficients
        object.  Coefficients::makeLowShelf() and friends allocate a fresh
        ref-counted object on every call, which is exactly what we must not do
        when a knob moves while the audio thread is running - this writes the
        five normalised floats into storage that was allocated in prepare().    */
    void setBiquad (Coeffs& dest, double b0, double b1, double b2,
                    double a0, double a1, double a2) noexcept
    {
        const auto inv = (a0 != 0.0 ? 1.0 / a0 : 1.0);
        auto* c = dest.getRawCoefficients();
        c[0] = (float) (b0 * inv);
        c[1] = (float) (b1 * inv);
        c[2] = (float) (b2 * inv);
        c[3] = (float) (a1 * inv);
        c[4] = (float) (a2 * inv);
    }

    void setPassThrough (Coeffs& dest) noexcept
    {
        setBiquad (dest, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    }

    struct Rbj { double cosw, alpha; };

    Rbj rbj (double freq, double q, double sr) noexcept
    {
        const auto f  = juce::jlimit (10.0, sr * 0.49, freq);
        const auto w0 = juce::MathConstants<double>::twoPi * f / sr;
        const auto s  = std::sin (w0);
        return { std::cos (w0), s / (2.0 * juce::jmax (0.05, q)) };
    }

    constexpr double butterworthQ = 0.70710678118654752;

    void makePeaking (Coeffs& c, double freq, double q, double gainDb, double sr) noexcept
    {
        const auto r = rbj (freq, q, sr);
        const auto A = std::pow (10.0, gainDb / 40.0);
        setBiquad (c, 1.0 + r.alpha * A, -2.0 * r.cosw, 1.0 - r.alpha * A,
                      1.0 + r.alpha / A, -2.0 * r.cosw, 1.0 - r.alpha / A);
    }

    void makeLowShelf (Coeffs& c, double freq, double gainDb, double sr) noexcept
    {
        const auto r  = rbj (freq, butterworthQ, sr);
        const auto A  = std::pow (10.0, gainDb / 40.0);
        const auto sq = 2.0 * std::sqrt (A) * r.alpha;
        setBiquad (c,       A * ((A + 1.0) - (A - 1.0) * r.cosw + sq),
                      2.0 * A * ((A - 1.0) - (A + 1.0) * r.cosw),
                            A * ((A + 1.0) - (A - 1.0) * r.cosw - sq),
                                ((A + 1.0) + (A - 1.0) * r.cosw + sq),
                         -2.0 * ((A - 1.0) + (A + 1.0) * r.cosw),
                                ((A + 1.0) + (A - 1.0) * r.cosw - sq));
    }

    void makeHighShelf (Coeffs& c, double freq, double gainDb, double sr) noexcept
    {
        const auto r  = rbj (freq, butterworthQ, sr);
        const auto A  = std::pow (10.0, gainDb / 40.0);
        const auto sq = 2.0 * std::sqrt (A) * r.alpha;
        setBiquad (c,        A * ((A + 1.0) + (A - 1.0) * r.cosw + sq),
                      -2.0 * A * ((A - 1.0) + (A + 1.0) * r.cosw),
                             A * ((A + 1.0) + (A - 1.0) * r.cosw - sq),
                                 ((A + 1.0) - (A - 1.0) * r.cosw + sq),
                           2.0 * ((A - 1.0) - (A + 1.0) * r.cosw),
                                 ((A + 1.0) - (A - 1.0) * r.cosw - sq));
    }

    void makeLowPass (Coeffs& c, double freq, double sr) noexcept
    {
        const auto r = rbj (freq, butterworthQ, sr);
        setBiquad (c, (1.0 - r.cosw) * 0.5, 1.0 - r.cosw, (1.0 - r.cosw) * 0.5,
                      1.0 + r.alpha, -2.0 * r.cosw, 1.0 - r.alpha);
    }

    void makeHighPass (Coeffs& c, double freq, double sr) noexcept
    {
        const auto r = rbj (freq, butterworthQ, sr);
        setBiquad (c, (1.0 + r.cosw) * 0.5, -(1.0 + r.cosw), (1.0 + r.cosw) * 0.5,
                      1.0 + r.alpha, -2.0 * r.cosw, 1.0 - r.alpha);
    }

    /** One-pole smoothing coefficient for a time constant in seconds. */
    float onePoleCoeff (double seconds, double sr) noexcept
    {
        if (seconds <= 0.0 || sr <= 0.0)
            return 1.0f;

        return (float) (1.0 - std::exp (-1.0 / (seconds * sr)));
    }

    /** Plain direct-form-I biquad with its own state, for the places where we
        need the filter to live outside juce::dsp (loudness K-weighting). */
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        double process (double x) noexcept
        {
            const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }

        void reset() noexcept { x1 = x2 = y1 = y2 = 0.0; }
    };

    //==============================================================================
    /*  Shared plumbing for every built-in effect: the parameter table, the
        NamedValueSet round-trip and a non-blocking hand-off of parameter changes
        from the message thread to the audio thread.                            */
    class FxBase : public BuiltinEffect
    {
    public:
        std::vector<ParamInfo> getParameterInfo() const override { return info; }

        void setParameters (const juce::NamedValueSet& newValues) override
        {
            {
                const juce::SpinLock::ScopedLockType sl (paramLock);

                for (size_t i = 0; i < info.size(); ++i)
                    if (const auto* v = newValues.getVarPointer (juce::Identifier (info[i].id)))
                        values[i] = juce::jlimit (info[i].min, info[i].max, (float) *v);
            }

            dirty.store (true);
        }

        juce::NamedValueSet getParameters() const override
        {
            const juce::SpinLock::ScopedLockType sl (paramLock);

            juce::NamedValueSet s;
            for (size_t i = 0; i < info.size(); ++i)
                s.set (juce::Identifier (info[i].id), (double) values[i]);

            return s;
        }

        void setParameterNormalised (int index, float normalised) noexcept override
        {
            if (! juce::isPositiveAndBelow (index, (int) info.size()))
                return;

            const juce::SpinLock::ScopedTryLockType sl (paramLock);

            if (! sl.isLocked())
                return;

            const auto& pi = info[(size_t) index];
            values[(size_t) index] = pi.min + (pi.max - pi.min) * juce::jlimit (0.0f, 1.0f, normalised);
            dirty.store (true);
        }

        void prepare (double newSampleRate, int newBlockSize, int newNumChannels) override
        {
            sampleRate  = newSampleRate > 0.0 ? newSampleRate : 44100.0;
            maxBlock    = juce::jmax (1, newBlockSize);
            numChannels = juce::jlimit (1, maxFxChannels, newNumChannels);

            prepareImpl();
            dirty.store (true);
            reset();
        }

    protected:
        int addParam (juce::String id, juce::String label, float minValue, float maxValue,
                      float defaultValue, juce::String suffix = {}, bool logarithmic = false)
        {
            info.push_back ({ std::move (id), std::move (label), minValue, maxValue,
                              defaultValue, std::move (suffix), logarithmic });
            values.push_back (defaultValue);
            return (int) info.size() - 1;
        }

        float p (int index) const noexcept { return values[(size_t) index]; }

        /** Call first thing in process().  Never blocks: if the message thread
            happens to be mid-write we simply keep last block's coefficients. */
        void applyPendingParameters()
        {
            if (! dirty.load())
                return;

            const juce::SpinLock::ScopedTryLockType sl (paramLock);

            if (! sl.isLocked())
                return;

            dirty.store (false);
            updateCoefficients();
        }

        virtual void prepareImpl() {}
        virtual void updateCoefficients() {}

        double sampleRate = 44100.0;
        int maxBlock = 512;
        int numChannels = 2;

    private:
        std::vector<ParamInfo> info;
        std::vector<float> values;
        mutable juce::SpinLock paramLock;
        std::atomic<bool> dirty { true };
    };

    //==============================================================================
    /*  4-band parametric EQ plus HP/LP (spec 8.4.6).                           */
    class EqEffect final : public FxBase
    {
    public:
        EqEffect()
        {
            pHpOn     = addParam ("hpOn",      "High Pass",     0.0f, 1.0f, 0.0f);
            pHpFreq   = addParam ("hpFreq",    "HP Freq",      20.0f, 2000.0f, 80.0f, "Hz", true);
            pLowGain  = addParam ("lowGain",   "Low Gain",    -24.0f, 24.0f, 0.0f, "dB");
            pLowFreq  = addParam ("lowFreq",   "Low Freq",     20.0f, 1000.0f, 120.0f, "Hz", true);
            pMid1Gain = addParam ("mid1Gain",  "Mid 1 Gain",  -24.0f, 24.0f, 0.0f, "dB");
            pMid1Freq = addParam ("mid1Freq",  "Mid 1 Freq",   40.0f, 8000.0f, 400.0f, "Hz", true);
            pMid1Q    = addParam ("mid1Q",     "Mid 1 Q",       0.1f, 10.0f, 1.0f, {}, true);
            pMid2Gain = addParam ("mid2Gain",  "Mid 2 Gain",  -24.0f, 24.0f, 0.0f, "dB");
            pMid2Freq = addParam ("mid2Freq",  "Mid 2 Freq",  200.0f, 16000.0f, 3000.0f, "Hz", true);
            pMid2Q    = addParam ("mid2Q",     "Mid 2 Q",       0.1f, 10.0f, 1.0f, {}, true);
            pHighGain = addParam ("highGain",  "High Gain",   -24.0f, 24.0f, 0.0f, "dB");
            pHighFreq = addParam ("highFreq",  "High Freq",  1000.0f, 20000.0f, 8000.0f, "Hz", true);
            pLpOn     = addParam ("lpOn",      "Low Pass",      0.0f, 1.0f, 0.0f);
            pLpFreq   = addParam ("lpFreq",    "LP Freq",    1000.0f, 20000.0f, 18000.0f, "Hz", true);
        }

        juce::String getType() const override { return "eq"; }

        void reset() override
        {
            for (auto& chain : filters)
                for (auto& f : chain)
                    f.reset();

            ring.fill (0.0f);
            ringWrite.store (0);
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), numChannels);

            for (int ch = 0; ch < nc; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                auto& chain = filters[(size_t) ch];

                for (int i = 0; i < n; ++i)
                {
                    auto x = d[i];
                    for (auto& f : chain)
                        x = f.processSample (x);
                    d[i] = x;
                }

                for (auto& f : chain)
                    f.snapToZero();
            }

            pushToAnalyser (buffer, nc, n);
        }

        /*  "response:<hz>"   - the EQ curve in dB at that frequency
            "spectrumBins"    - how many analyser bins there are
            "spectrum:<bin>"  - analyser magnitude in dB for that bin           */
        float getMeter (const juce::String& meterId) const override
        {
            if (meterId.startsWith ("response:"))
            {
                const auto hz = (double) meterId.fromFirstOccurrenceOf (":", false, false).getFloatValue();
                double db = 0.0;

                for (const auto& c : bandCoeffs)
                    if (c != nullptr)
                        db += juce::Decibels::gainToDecibels (c->getMagnitudeForFrequency (hz, sampleRate), -120.0);

                return (float) db;
            }

            if (meterId == "spectrumBins")
                return (float) numAnalyserBins;

            if (meterId.startsWith ("spectrum:"))
            {
                refreshAnalyser();
                const auto bin = meterId.fromFirstOccurrenceOf (":", false, false).getIntValue();
                return juce::isPositiveAndBelow (bin, numAnalyserBins) ? bins[(size_t) bin] : -100.0f;
            }

            return 0.0f;
        }

    private:
        static constexpr int numBands = 6;   // HP, low shelf, mid 1, mid 2, high shelf, LP
        static constexpr int fftOrder = 10;
        static constexpr int fftSize  = 1 << fftOrder;
        static constexpr int numAnalyserBins = fftSize / 2;

        void prepareImpl() override
        {
            for (auto& c : bandCoeffs)
                if (c == nullptr)
                    c = makeBiquadHolder();

            for (auto& chain : filters)
                for (int b = 0; b < numBands; ++b)
                {
                    chain[(size_t) b].coefficients = bandCoeffs[(size_t) b];
                    chain[(size_t) b].reset();
                }
        }

        void updateCoefficients() override
        {
            if (bandCoeffs[0] == nullptr)
                return;

            if (p (pHpOn) >= 0.5f) makeHighPass (*bandCoeffs[0], p (pHpFreq), sampleRate);
            else                   setPassThrough (*bandCoeffs[0]);

            makeLowShelf  (*bandCoeffs[1], p (pLowFreq),  p (pLowGain),  sampleRate);
            makePeaking   (*bandCoeffs[2], p (pMid1Freq), p (pMid1Q), p (pMid1Gain), sampleRate);
            makePeaking   (*bandCoeffs[3], p (pMid2Freq), p (pMid2Q), p (pMid2Gain), sampleRate);
            makeHighShelf (*bandCoeffs[4], p (pHighFreq), p (pHighGain), sampleRate);

            if (p (pLpOn) >= 0.5f) makeLowPass (*bandCoeffs[5], p (pLpFreq), sampleRate);
            else                   setPassThrough (*bandCoeffs[5]);
        }

        void pushToAnalyser (const juce::AudioBuffer<float>& buffer, int nc, int n) noexcept
        {
            auto w = ringWrite.load();

            for (int i = 0; i < n; ++i)
            {
                float sum = 0.0f;
                for (int ch = 0; ch < nc; ++ch)
                    sum += buffer.getSample (ch, i);

                ring[(size_t) w] = nc > 0 ? sum / (float) nc : 0.0f;
                w = (w + 1) & (fftSize - 1);
            }

            ringWrite.store (w);
        }

        /*  ponytail: the analyser reads the ring while the audio thread writes it,
            so a redraw can straddle a block boundary.  It is a display only - swap
            in a juce::AbstractFifo if the smearing ever becomes visible.          */
        void refreshAnalyser() const
        {
            const auto now = juce::Time::getMillisecondCounter();

            if (now - lastAnalyserMs < 40 && lastAnalyserMs != 0)
                return;

            lastAnalyserMs = now;

            const auto w = ringWrite.load();
            for (int i = 0; i < fftSize; ++i)
            {
                const auto window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                            * (float) i / (float) (fftSize - 1));
                fftScratch[(size_t) i] = ring[(size_t) ((w + i) & (fftSize - 1))] * window;
            }

            std::fill (fftScratch.begin() + fftSize, fftScratch.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (fftScratch.data(), true);

            for (int i = 0; i < numAnalyserBins; ++i)
                bins[(size_t) i] = juce::Decibels::gainToDecibels (fftScratch[(size_t) i]
                                                                     * (2.0f / (float) fftSize), -100.0f);
        }

        std::array<Coeffs::Ptr, numBands> bandCoeffs;
        std::array<std::array<Iir, numBands>, maxFxChannels> filters;

        std::array<float, fftSize> ring {};
        std::atomic<int> ringWrite { 0 };
        mutable juce::dsp::FFT fft { fftOrder };
        mutable std::array<float, fftSize * 2> fftScratch {};
        mutable std::array<float, numAnalyserBins> bins {};
        mutable juce::uint32 lastAnalyserMs = 0;

        int pHpOn, pHpFreq, pLowGain, pLowFreq, pMid1Gain, pMid1Freq, pMid1Q,
            pMid2Gain, pMid2Freq, pMid2Q, pHighGain, pHighFreq, pLpOn, pLpFreq;
    };

    //==============================================================================
    /*  Soft-knee compressor with a linked-stereo peak detector.                */
    class CompressorEffect final : public FxBase
    {
    public:
        CompressorEffect()
        {
            pThreshold = addParam ("threshold", "Threshold", -60.0f, 0.0f, -18.0f, "dB");
            pRatio     = addParam ("ratio",     "Ratio",       1.0f, 20.0f, 4.0f, ":1", true);
            pAttack    = addParam ("attack",    "Attack",      0.1f, 200.0f, 10.0f, "ms", true);
            pRelease   = addParam ("release",   "Release",     5.0f, 2000.0f, 120.0f, "ms", true);
            pKnee      = addParam ("knee",      "Knee",        0.0f, 24.0f, 6.0f, "dB");
            pMakeup    = addParam ("makeup",    "Makeup",    -12.0f, 24.0f, 0.0f, "dB");
            pMix       = addParam ("mix",       "Mix",         0.0f, 1.0f, 1.0f);
        }

        juce::String getType() const override { return "compressor"; }

        void reset() override
        {
            envDb = 0.0f;
            gainReductionDb.store (0.0f);
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), maxFxChannels);
            auto* const* d = buffer.getArrayOfWritePointers();

            const auto threshold = p (pThreshold);
            const auto knee      = p (pKnee);
            const auto slope     = 1.0f / juce::jmax (1.0f, p (pRatio)) - 1.0f;   // <= 0
            const auto makeup    = p (pMakeup);
            const auto mix       = p (pMix);
            float worst = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < nc; ++ch)
                    peak = juce::jmax (peak, std::abs (d[ch][i]));

                const auto over = juce::Decibels::gainToDecibels (peak, -120.0f) - threshold;

                float target = 0.0f;                                   // dB of reduction, <= 0
                if (knee > 0.0f && over > -knee * 0.5f && over < knee * 0.5f)
                {
                    const auto x = over + knee * 0.5f;
                    target = slope * x * x / (2.0f * knee);
                }
                else if (over >= knee * 0.5f)
                {
                    target = slope * over;
                }

                envDb += (target - envDb) * (target < envDb ? attackCoeff : releaseCoeff);

                const auto g = juce::Decibels::decibelsToGain (envDb + makeup);
                const auto applied = (1.0f - mix) + mix * g;

                for (int ch = 0; ch < nc; ++ch)
                    d[ch][i] *= applied;

                worst = juce::jmin (worst, envDb);
            }

            gainReductionDb.store (-worst);
        }

        float getMeter (const juce::String& meterId) const override
        {
            return meterId == "gainReduction" ? gainReductionDb.load() : 0.0f;
        }

    private:
        void updateCoefficients() override
        {
            attackCoeff  = onePoleCoeff (p (pAttack)  * 0.001, sampleRate);
            releaseCoeff = onePoleCoeff (p (pRelease) * 0.001, sampleRate);
        }

        float envDb = 0.0f, attackCoeff = 1.0f, releaseCoeff = 1.0f;
        std::atomic<float> gainReductionDb { 0.0f };
        int pThreshold, pRatio, pAttack, pRelease, pKnee, pMakeup, pMix;
    };

    //==============================================================================
    /*  Noise gate with hold.                                                   */
    class GateEffect final : public FxBase
    {
    public:
        GateEffect()
        {
            pThreshold = addParam ("threshold", "Threshold", -80.0f, 0.0f, -40.0f, "dB");
            pRange     = addParam ("range",     "Range",     -80.0f, 0.0f, -60.0f, "dB");
            pAttack    = addParam ("attack",    "Attack",      0.1f, 100.0f, 1.0f, "ms", true);
            pHold      = addParam ("hold",      "Hold",        0.0f, 500.0f, 50.0f, "ms");
            pRelease   = addParam ("release",   "Release",     5.0f, 2000.0f, 150.0f, "ms", true);
        }

        juce::String getType() const override { return "gate"; }

        void reset() override
        {
            envDb = p (pRange);
            holdCounter = 0;
            gainReductionDb.store (0.0f);
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), maxFxChannels);
            auto* const* d = buffer.getArrayOfWritePointers();

            const auto threshold = p (pThreshold);
            const auto range     = p (pRange);
            float worst = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < nc; ++ch)
                    peak = juce::jmax (peak, std::abs (d[ch][i]));

                const auto level = juce::Decibels::gainToDecibels (peak, -120.0f);

                float target = range;
                if (level > threshold)      { holdCounter = holdSamples; target = 0.0f; }
                else if (holdCounter > 0)   { --holdCounter;             target = 0.0f; }

                envDb += (target - envDb) * (target > envDb ? attackCoeff : releaseCoeff);

                const auto g = juce::Decibels::decibelsToGain (envDb);
                for (int ch = 0; ch < nc; ++ch)
                    d[ch][i] *= g;

                worst = juce::jmin (worst, envDb);
            }

            gainReductionDb.store (-worst);
        }

        float getMeter (const juce::String& meterId) const override
        {
            return meterId == "gainReduction" ? gainReductionDb.load() : 0.0f;
        }

    private:
        void updateCoefficients() override
        {
            attackCoeff  = onePoleCoeff (p (pAttack)  * 0.001, sampleRate);
            releaseCoeff = onePoleCoeff (p (pRelease) * 0.001, sampleRate);
            holdSamples  = (int) (p (pHold) * 0.001 * sampleRate);
        }

        float envDb = -60.0f, attackCoeff = 1.0f, releaseCoeff = 1.0f;
        int holdSamples = 0, holdCounter = 0;
        std::atomic<float> gainReductionDb { 0.0f };
        int pThreshold, pRange, pAttack, pHold, pRelease;
    };

    //==============================================================================
    /*  Look-ahead brickwall limiter.  The envelope is taken from the undelayed
        signal and applied to the delayed one, so the gain is already down by the
        time the peak arrives - no per-sample sliding maximum needed.

        ponytail: detection is sample-peak, not ITU true-peak, so inter-sample
        overs can still sneak past the ceiling by a few tenths of a dB.  The
        loudnessMeter effect measures the real true-peak; if its readout shows
        overs, run the detector through a 4x oversampler here too.               */
    class LimiterEffect final : public FxBase
    {
    public:
        LimiterEffect()
        {
            pInput     = addParam ("input",     "Input",     -12.0f, 24.0f, 0.0f, "dB");
            pCeiling   = addParam ("ceiling",   "Ceiling",   -12.0f, 0.0f, -0.3f, "dB");
            pRelease   = addParam ("release",   "Release",     1.0f, 1000.0f, 50.0f, "ms", true);
            pLookahead = addParam ("lookahead", "Lookahead",   0.0f, 10.0f, 3.0f, "ms");
        }

        juce::String getType() const override { return "limiter"; }

        void reset() override
        {
            delayBuffer.clear();
            writeIndex = 0;
            env = 0.0f;
            gainReductionDb.store (0.0f);
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), delayBuffer.getNumChannels());
            const auto capacity = delayBuffer.getNumSamples();

            if (capacity <= 0 || nc <= 0)
                return;

            auto* const* d = buffer.getArrayOfWritePointers();
            const auto inGain  = juce::Decibels::decibelsToGain (p (pInput));
            const auto ceiling = juce::Decibels::decibelsToGain (p (pCeiling));
            float minGain = 1.0f;

            for (int i = 0; i < n; ++i)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < nc; ++ch)
                    peak = juce::jmax (peak, std::abs (d[ch][i] * inGain));

                env += (peak - env) * (peak > env ? attackCoeff : releaseCoeff);

                const auto g = env > ceiling ? ceiling / env : 1.0f;
                minGain = juce::jmin (minGain, g);

                const auto readIndex = (writeIndex + capacity - lookaheadSamples) % capacity;

                for (int ch = 0; ch < nc; ++ch)
                {
                    auto* line = delayBuffer.getWritePointer (ch);
                    line[writeIndex] = d[ch][i] * inGain;
                    d[ch][i] = juce::jlimit (-ceiling, ceiling, line[readIndex] * g);
                }

                writeIndex = (writeIndex + 1) % capacity;
            }

            gainReductionDb.store (-juce::Decibels::gainToDecibels (minGain, -60.0f));
        }

        float getMeter (const juce::String& meterId) const override
        {
            return meterId == "gainReduction" ? gainReductionDb.load() : 0.0f;
        }

    private:
        void prepareImpl() override
        {
            const auto maxLookahead = (int) (0.010 * sampleRate) + 2;
            delayBuffer.setSize (numChannels, maxLookahead, false, true, false);
        }

        void updateCoefficients() override
        {
            lookaheadSamples = juce::jlimit (0, juce::jmax (0, delayBuffer.getNumSamples() - 1),
                                             (int) (p (pLookahead) * 0.001 * sampleRate));

            // Reach the target within roughly the look-ahead window.
            attackCoeff  = lookaheadSamples > 0
                             ? onePoleCoeff ((double) lookaheadSamples / (3.0 * sampleRate), sampleRate)
                             : 1.0f;
            releaseCoeff = onePoleCoeff (p (pRelease) * 0.001, sampleRate);
        }

        juce::AudioBuffer<float> delayBuffer;
        int writeIndex = 0, lookaheadSamples = 0;
        float env = 0.0f, attackCoeff = 1.0f, releaseCoeff = 1.0f;
        std::atomic<float> gainReductionDb { 0.0f };
        int pInput, pCeiling, pRelease, pLookahead;
    };

    //==============================================================================
    /*  tanh soft-clipper with a 1 kHz tilt tone control.                        */
    class SaturatorEffect final : public FxBase
    {
    public:
        SaturatorEffect()
        {
            pDrive  = addParam ("drive",  "Drive",   0.0f, 36.0f, 6.0f, "dB");
            pTone   = addParam ("tone",   "Tone",   -1.0f, 1.0f, 0.0f);
            pMix    = addParam ("mix",    "Mix",     0.0f, 1.0f, 1.0f);
            pOutput = addParam ("output", "Output", -24.0f, 12.0f, 0.0f, "dB");
        }

        juce::String getType() const override { return "saturator"; }

        void reset() override { tiltState.fill (0.0f); }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), maxFxChannels);
            auto* const* d = buffer.getArrayOfWritePointers();

            // Normalised so full-scale in stays full-scale out; low-level material
            // gets a couple of dB of lift at low drive settings - trim with Output.
            const auto drive = juce::Decibels::decibelsToGain (p (pDrive));
            const auto norm  = 1.0f / std::tanh (drive);
            const auto tone  = p (pTone);
            const auto lowGain  = juce::Decibels::decibelsToGain (-tone * 6.0f);
            const auto highGain = juce::Decibels::decibelsToGain ( tone * 6.0f);
            const auto mix = p (pMix);
            const auto out = juce::Decibels::decibelsToGain (p (pOutput));

            for (int ch = 0; ch < nc; ++ch)
            {
                auto* s = d[ch];
                auto& lp = tiltState[(size_t) ch];

                for (int i = 0; i < n; ++i)
                {
                    const auto dry = s[i];
                    auto wet = std::tanh (dry * drive) * norm;

                    lp += (wet - lp) * tiltCoeff;
                    wet = lp * lowGain + (wet - lp) * highGain;

                    s[i] = (dry * (1.0f - mix) + wet * mix) * out;
                }
            }
        }

    private:
        void updateCoefficients() override
        {
            tiltCoeff = onePoleCoeff (1.0 / (juce::MathConstants<double>::twoPi * 1000.0), sampleRate);
        }

        std::array<float, maxFxChannels> tiltState {};
        float tiltCoeff = 0.1f;
        int pDrive, pTone, pMix, pOutput;
    };

    //==============================================================================
    /*  juce::dsp::Reverb with a pre-delay in front of the wet path.            */
    class ReverbEffect final : public FxBase
    {
    public:
        ReverbEffect()
        {
            pWet      = addParam ("wet",      "Wet",       0.0f, 1.0f, 0.3f);
            pDry      = addParam ("dry",      "Dry",       0.0f, 1.0f, 1.0f);
            pSize     = addParam ("size",     "Size",      0.0f, 1.0f, 0.5f);
            pDamping  = addParam ("damping",  "Damping",   0.0f, 1.0f, 0.5f);
            pWidth    = addParam ("width",    "Width",     0.0f, 1.0f, 1.0f);
            pPredelay = addParam ("predelay", "Pre-delay", 0.0f, 200.0f, 0.0f, "ms");
        }

        juce::String getType() const override { return "reverb"; }

        void reset() override
        {
            reverb.reset();
            predelay.reset();
            wetBuffer.clear();
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            // juce::dsp::Reverb only understands mono and stereo.
            const auto nc = juce::jmin (buffer.getNumChannels(), 2, wetBuffer.getNumChannels());

            if (nc <= 0 || n > wetBuffer.getNumSamples())
                return;

            for (int ch = 0; ch < nc; ++ch)
            {
                const auto* src = buffer.getReadPointer (ch);
                auto* dst = wetBuffer.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                {
                    predelay.pushSample (ch, src[i]);
                    dst[i] = predelay.popSample (ch);
                }
            }

            juce::dsp::AudioBlock<float> block (wetBuffer.getArrayOfWritePointers(),
                                                (size_t) nc, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> context (block);
            reverb.process (context);

            const auto wet = p (pWet);
            const auto dry = p (pDry);

            for (int ch = 0; ch < nc; ++ch)
            {
                buffer.applyGain (ch, 0, n, dry);
                buffer.addFrom (ch, 0, wetBuffer, ch, 0, n, wet);
            }
        }

    private:
        void prepareImpl() override
        {
            const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock,
                                                (juce::uint32) juce::jmax (1, numChannels) };
            reverb.prepare (spec);

            predelay.setMaximumDelayInSamples ((int) (0.2 * sampleRate) + 2);
            predelay.prepare (spec);

            wetBuffer.setSize (juce::jmax (1, numChannels), maxBlock, false, true, false);
        }

        void updateCoefficients() override
        {
            juce::dsp::Reverb::Parameters params;
            params.roomSize   = p (pSize);
            params.damping    = p (pDamping);
            params.width      = p (pWidth);
            params.wetLevel   = 1.0f;      // dry/wet balance is done by hand below,
            params.dryLevel   = 0.0f;      // so the pre-delay only hits the wet path
            params.freezeMode = 0.0f;
            reverb.setParameters (params);

            predelay.setDelay ((float) juce::jmin ((double) p (pPredelay) * 0.001 * sampleRate,
                                                   0.2 * sampleRate));
        }

        juce::dsp::Reverb reverb;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> predelay;
        juce::AudioBuffer<float> wetBuffer;
        int pWet, pDry, pSize, pDamping, pWidth, pPredelay;
    };

    //==============================================================================
    /*  Tempo-syncable stereo delay with a filtered feedback path.              */
    class DelayEffect final : public FxBase
    {
    public:
        DelayEffect()
        {
            pSync     = addParam ("sync",     "Tempo Sync",  0.0f, 1.0f, 0.0f);
            pTime     = addParam ("time",     "Time",        1.0f, 2000.0f, 350.0f, "ms", true);
            pDivision = addParam ("division", "Division",    0.0f, 8.0f, 3.0f);
            pFeedback = addParam ("feedback", "Feedback",    0.0f, 0.95f, 0.35f);
            pMix      = addParam ("mix",      "Mix",         0.0f, 1.0f, 0.3f);
            pDamping  = addParam ("damping",  "Damping",     0.0f, 1.0f, 0.4f);
            pLowCut   = addParam ("lowCut",   "Low Cut",    20.0f, 1000.0f, 120.0f, "Hz", true);
            pPingPong = addParam ("pingPong", "Ping-Pong",   0.0f, 1.0f, 0.0f);
        }

        juce::String getType() const override { return "delay"; }

        void setTempoBpm (double newBpm) override
        {
            if (newBpm > 1.0)
                bpm.store (newBpm);
        }

        void reset() override
        {
            line.reset();
            lpState.fill (0.0f);
            hpState.fill (0.0f);
            delaySmooth.setCurrentAndTargetValue (targetDelaySamples());
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), 2);

            if (nc <= 0)
                return;

            auto* const* d = buffer.getArrayOfWritePointers();
            delaySmooth.setTargetValue (targetDelaySamples());

            const auto feedback = p (pFeedback);
            const auto mix      = p (pMix);
            const auto pingPong = p (pPingPong) >= 0.5f && nc == 2;

            for (int i = 0; i < n; ++i)
            {
                const auto delaySamples = delaySmooth.getNextValue();

                float in[2] { 0.0f, 0.0f }, wet[2] { 0.0f, 0.0f };

                for (int ch = 0; ch < nc; ++ch)
                {
                    in[ch] = d[ch][i];
                    auto tap = line.popSample (ch, delaySamples, true);

                    // damping = low-pass, lowCut = high-pass, both inside the loop
                    auto& lp = lpState[(size_t) ch];
                    auto& hp = hpState[(size_t) ch];
                    lp += (tap - lp) * dampingCoeff;
                    tap = lp;
                    hp += (tap - hp) * lowCutCoeff;
                    wet[ch] = tap - hp;
                }

                if (pingPong)
                {
                    line.pushSample (0, (in[0] + in[1]) * 0.5f + wet[1] * feedback);
                    line.pushSample (1, wet[0] * feedback);
                }
                else
                {
                    for (int ch = 0; ch < nc; ++ch)
                        line.pushSample (ch, in[ch] + wet[ch] * feedback);
                }

                for (int ch = 0; ch < nc; ++ch)
                    d[ch][i] = in[ch] * (1.0f - mix) + wet[ch] * mix;
            }
        }

    private:
        void prepareImpl() override
        {
            const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock,
                                                (juce::uint32) juce::jmax (2, numChannels) };
            line.setMaximumDelayInSamples ((int) (maxDelaySeconds * sampleRate) + 4);
            line.prepare (spec);
            delaySmooth.reset (sampleRate, 0.05);
        }

        void updateCoefficients() override
        {
            // 0 damping = open, 1 = very dark.
            const auto lpHz = 18000.0 - 17500.0 * (double) p (pDamping);
            dampingCoeff = onePoleCoeff (1.0 / (juce::MathConstants<double>::twoPi * juce::jmax (200.0, lpHz)), sampleRate);
            lowCutCoeff  = onePoleCoeff (1.0 / (juce::MathConstants<double>::twoPi * juce::jmax (10.0, (double) p (pLowCut))), sampleRate);
        }

        float targetDelaySamples() const noexcept
        {
            double seconds;

            if (p (pSync) >= 0.5f)
            {
                static constexpr double beatsPerDivision[] { 4.0, 2.0, 1.0, 0.5, 0.25,
                                                             2.0 / 3.0, 1.0 / 3.0, 1.5, 0.75 };
                const auto index = juce::jlimit (0, (int) juce::numElementsInArray (beatsPerDivision) - 1,
                                                 (int) std::lround (p (pDivision)));
                seconds = beatsPerDivision[index] * 60.0 / bpm.load();
            }
            else
            {
                seconds = (double) p (pTime) * 0.001;
            }

            return (float) juce::jlimit (1.0, maxDelaySeconds * sampleRate, seconds * sampleRate);
        }

        static constexpr double maxDelaySeconds = 2.0;

        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line;
        juce::SmoothedValue<float> delaySmooth { 1.0f };
        std::array<float, 2> lpState {}, hpState {};
        float dampingCoeff = 1.0f, lowCutCoeff = 0.0f;
        std::atomic<double> bpm { 120.0 };
        int pSync, pTime, pDivision, pFeedback, pMix, pDamping, pLowCut, pPingPong;
    };

    //==============================================================================
    /*  ITU-R BS.1770 style loudness meter (spec 8.4.6, 11-P1).  Passes audio
        through untouched and publishes momentary / short-term / integrated LUFS
        plus true-peak for the mastering meters and the streaming export presets.

        Structure follows libebur128: K-weight, accumulate 100 ms sub-blocks, then
        derive the 400 ms and 3 s windows from a ring of those, and gate the
        integrated value through a histogram so nothing allocates while running. */
    class LoudnessMeterEffect final : public FxBase
    {
    public:
        LoudnessMeterEffect()
        {
            pTarget = addParam ("target", "Target", -24.0f, -6.0f, -14.0f, "LUFS");

            for (int i = 0; i < histBins; ++i)
                binEnergy[(size_t) i] = std::pow (10.0, (binLoudness (i) + 0.691) / 10.0);
        }

        juce::String getType() const override { return "loudnessMeter"; }

        void reset() override
        {
            for (auto& ch : preFilter) ch.reset();
            for (auto& ch : rlbFilter) ch.reset();

            for (auto& ch : ring) ch.fill (0.0);
            ringWrite = 0;
            ringFilled = 0;

            for (auto& s : subBlockSum) s = 0.0;
            subBlockCount = 0;

            histogram.fill (0);
            gatedBlocks = 0;

            momentary.store (-100.0f);
            shortTerm.store (-100.0f);
            integrated.store (-100.0f);
            truePeakDb.store (-100.0f);
            peakDb.store (-100.0f);
            rmsDb.store (-100.0f);
        }

        void process (juce::AudioBuffer<float>& buffer) override
        {
            applyPendingParameters();

            const auto n  = buffer.getNumSamples();
            const auto nc = juce::jmin (buffer.getNumChannels(), numChannels);

            if (n <= 0 || nc <= 0)
                return;

            // --- plain peak / RMS -------------------------------------------
            float blockPeak = 0.0f;
            double squareSum = 0.0;

            for (int ch = 0; ch < nc; ++ch)
            {
                blockPeak = juce::jmax (blockPeak, buffer.getMagnitude (ch, 0, n));
                const auto r = buffer.getRMSLevel (ch, 0, n);
                squareSum += (double) r * r;
            }

            peakDb.store (juce::Decibels::gainToDecibels (blockPeak, -100.0f));
            rmsDb.store (juce::Decibels::gainToDecibels ((float) std::sqrt (squareSum / nc), -100.0f));

            // --- true peak via 4x oversampling ------------------------------
            if (oversampler != nullptr && n <= maxBlock)
            {
                juce::dsp::AudioBlock<const float> in (buffer.getArrayOfReadPointers(),
                                                       (size_t) nc, (size_t) n);
                auto up = oversampler->processSamplesUp (in);

                // The oversampler hands back all of its channels, but only the
                // first `nc` of them were fed this block.
                float tp = 0.0f;
                for (size_t ch = 0; ch < juce::jmin (up.getNumChannels(), (size_t) nc); ++ch)
                {
                    const auto* s = up.getChannelPointer (ch);
                    for (size_t i = 0; i < up.getNumSamples(); ++i)
                        tp = juce::jmax (tp, std::abs (s[i]));
                }

                truePeakDb.store (juce::jmax (truePeakDb.load(),
                                              juce::Decibels::gainToDecibels (tp, -100.0f)));
            }

            // --- K-weighted sub-block accumulation --------------------------
            const auto* const* src = buffer.getArrayOfReadPointers();

            for (int i = 0; i < n; ++i)
            {
                for (int ch = 0; ch < nc; ++ch)
                {
                    const auto k = rlbFilter[(size_t) ch].process (
                                       preFilter[(size_t) ch].process ((double) src[ch][i]));
                    subBlockSum[(size_t) ch] += k * k;
                }

                if (++subBlockCount >= subBlockSamples)
                {
                    closeSubBlock (nc);
                    subBlockCount = 0;
                }
            }
        }

        float getMeter (const juce::String& meterId) const override
        {
            if (meterId == "momentary")  return momentary.load();
            if (meterId == "shortTerm")  return shortTerm.load();
            if (meterId == "integrated") return integrated.load();
            if (meterId == "truePeak")   return truePeakDb.load();
            if (meterId == "peak")       return peakDb.load();
            if (meterId == "rms")        return rmsDb.load();
            if (meterId == "target")     return p (pTarget);
            return 0.0f;
        }

    private:
        static constexpr int ringLength = 30;    // 30 x 100 ms = 3 s (short-term)
        static constexpr int histBins   = 1000;
        static constexpr double histMin = -70.0, histMax = 5.0;

        static double binLoudness (int index) noexcept
        {
            return histMin + (histMax - histMin) * ((double) index + 0.5) / (double) histBins;
        }

        void prepareImpl() override
        {
            subBlockSamples = juce::jmax (1, (int) (sampleRate / 10.0));

            makeKWeighting();

            oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
                              (size_t) juce::jmax (1, numChannels), 2,
                              juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
            oversampler->initProcessing ((size_t) maxBlock);
        }

        /*  BS.1770 pre-filter + RLB high-pass, derived analytically so they are
            correct at any sample rate (constants from the ITU recommendation).  */
        void makeKWeighting()
        {
            {
                constexpr double f0 = 1681.974450955533;
                constexpr double G  = 3.999843853973347;
                constexpr double Q  = 0.7071752369554196;

                const auto K  = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
                const auto Vh = std::pow (10.0, G / 20.0);
                const auto Vb = std::pow (Vh, 0.4996667741545416);
                const auto a0 = 1.0 + K / Q + K * K;

                Biquad pre;
                pre.b0 = (Vh + Vb * K / Q + K * K) / a0;
                pre.b1 = 2.0 * (K * K - Vh) / a0;
                pre.b2 = (Vh - Vb * K / Q + K * K) / a0;
                pre.a1 = 2.0 * (K * K - 1.0) / a0;
                pre.a2 = (1.0 - K / Q + K * K) / a0;

                for (auto& ch : preFilter) ch = pre;
            }

            {
                constexpr double f0 = 38.13547087602444;
                constexpr double Q  = 0.5003270373238773;

                const auto K  = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
                const auto a0 = 1.0 + K / Q + K * K;

                Biquad rlb;
                rlb.b0 =  1.0;
                rlb.b1 = -2.0;
                rlb.b2 =  1.0;
                rlb.a1 = 2.0 * (K * K - 1.0) / a0;
                rlb.a2 = (1.0 - K / Q + K * K) / a0;

                for (auto& ch : rlbFilter) ch = rlb;
            }
        }

        static double channelWeight (int channel) noexcept
        {
            return channel >= 3 ? 1.41 : 1.0;   // BS.1770 surround weighting
        }

        /** Mean of the last `blocks` sub-blocks, as a gated-window loudness. */
        double windowLoudness (int blocks, int nc) const noexcept
        {
            const auto available = juce::jmin (blocks, ringFilled);

            if (available <= 0)
                return -100.0;

            double sum = 0.0;

            for (int ch = 0; ch < nc; ++ch)
            {
                double chSum = 0.0;
                for (int b = 0; b < available; ++b)
                {
                    const auto index = (ringWrite - 1 - b + ringLength * 2) % ringLength;
                    chSum += ring[(size_t) ch][(size_t) index];
                }

                sum += channelWeight (ch) * chSum / (double) available;
            }

            return sum > 0.0 ? -0.691 + 10.0 * std::log10 (sum) : -100.0;
        }

        void closeSubBlock (int nc) noexcept
        {
            for (int ch = 0; ch < nc; ++ch)
            {
                ring[(size_t) ch][(size_t) ringWrite] = subBlockSum[(size_t) ch] / (double) subBlockSamples;
                subBlockSum[(size_t) ch] = 0.0;
            }

            ringWrite = (ringWrite + 1) % ringLength;
            ringFilled = juce::jmin (ringLength, ringFilled + 1);

            const auto m = windowLoudness (4, nc);      // 400 ms
            momentary.store ((float) m);
            shortTerm.store ((float) windowLoudness (ringLength, nc));

            // A gating block is one 400 ms window every 100 ms - i.e. exactly one
            // per sub-block once the ring has four of them.
            if (ringFilled >= 4 && m >= histMin)
            {
                const auto bin = juce::jlimit (0, histBins - 1,
                                               (int) ((m - histMin) / (histMax - histMin) * histBins));
                ++histogram[(size_t) bin];
                ++gatedBlocks;
                integrated.store ((float) computeIntegrated());
            }
        }

        double computeIntegrated() const noexcept
        {
            if (gatedBlocks == 0)
                return -100.0;

            double sum = 0.0;
            juce::int64 count = 0;

            for (int i = 0; i < histBins; ++i)
                if (histogram[(size_t) i] != 0)
                {
                    sum += binEnergy[(size_t) i] * histogram[(size_t) i];
                    count += histogram[(size_t) i];
                }

            if (count == 0 || sum <= 0.0)
                return -100.0;

            const auto relativeGate = -0.691 + 10.0 * std::log10 (sum / (double) count) - 10.0;

            double sum2 = 0.0;
            juce::int64 count2 = 0;

            for (int i = 0; i < histBins; ++i)
                if (histogram[(size_t) i] != 0 && binLoudness (i) >= relativeGate)
                {
                    sum2 += binEnergy[(size_t) i] * histogram[(size_t) i];
                    count2 += histogram[(size_t) i];
                }

            if (count2 == 0 || sum2 <= 0.0)
                return -100.0;

            return -0.691 + 10.0 * std::log10 (sum2 / (double) count2);
        }

        std::array<Biquad, maxFxChannels> preFilter, rlbFilter;
        std::array<std::array<double, ringLength>, maxFxChannels> ring {};
        std::array<double, maxFxChannels> subBlockSum {};
        std::array<double, histBins> binEnergy {};
        std::array<juce::uint32, histBins> histogram {};

        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

        int subBlockSamples = 4410, subBlockCount = 0;
        int ringWrite = 0, ringFilled = 0;
        juce::int64 gatedBlocks = 0;

        std::atomic<float> momentary { -100.0f }, shortTerm { -100.0f }, integrated { -100.0f },
                           truePeakDb { -100.0f }, peakDb { -100.0f }, rmsDb { -100.0f };
        int pTarget;
    };

    struct TypeEntry { const char* id; const char* displayName; };

    const TypeEntry builtinTypes[]
    {
        { "eq",            "Parametric EQ" },
        { "compressor",    "Compressor" },
        { "gate",          "Noise Gate" },
        { "limiter",       "Limiter" },
        { "saturator",     "Saturator" },
        { "reverb",        "Reverb" },
        { "delay",         "Delay" },
        { "loudnessMeter", "Loudness Meter" }
    };
}

//==============================================================================
std::unique_ptr<BuiltinEffect> createBuiltinEffect (const juce::String& type)
{
    if (type == "eq")            return std::make_unique<EqEffect>();
    if (type == "compressor")    return std::make_unique<CompressorEffect>();
    if (type == "gate")          return std::make_unique<GateEffect>();
    if (type == "limiter")       return std::make_unique<LimiterEffect>();
    if (type == "saturator")     return std::make_unique<SaturatorEffect>();
    if (type == "reverb")        return std::make_unique<ReverbEffect>();
    if (type == "delay")         return std::make_unique<DelayEffect>();
    if (type == "loudnessMeter") return std::make_unique<LoudnessMeterEffect>();

    return {};
}

juce::StringArray getBuiltinEffectTypes()
{
    juce::StringArray types;

    for (const auto& e : builtinTypes)
        types.add (e.id);

    return types;
}

juce::String getBuiltinEffectDisplayName (const juce::String& type)
{
    for (const auto& e : builtinTypes)
        if (type == e.id)
            return e.displayName;

    return type;
}

}
