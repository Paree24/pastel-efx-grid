#include "FxCommon.h"

// Analysis: audio passes untouched; these fill VizTaps for the editor.

namespace efx
{
namespace
{

// ------------------------------------------------------------------ Spectrum
// Range, Speed, Tilt, Peak | Bars / Line
class Spectrum : public FxImpl
{
public:
    void prepare (double) override { tap.prepare (512); reset(); }
    void reset() override
    {
        tap.reset();
        for (int b = 0; b < VizTaps::kSpec; ++b) { taps->spec[(size_t) b].store (-120.0f); taps->specPeak[(size_t) b].store (-120.0f); }
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        static constexpr float kRel[3] = { 0.35f, 0.18f, 0.07f };
        tap.push (L, R, n, sr, *taps, kRel[juce::jlimit (0, 2, ci (c, 1))], c.p[2], on (c, 3));
    }
private:
    SpectrumTap tap;
};

// ------------------------------------------------------------------ Oscilloscope (GridEffect fills longScope)
class Scope : public FxImpl
{
public:
    void prepare (double) override {}
    void reset() override {}
    void process (float*, float*, int, const FxContext&) override {}
};

// ------------------------------------------------------------------ Loudness (ITU-R BS.1770 / EBU R128)
// Target, Scale, Reset | Mom / Short / Int
class Loudness : public FxImpl
{
public:
    void prepare (double s) override
    {
        for (int ch = 0; ch < 2; ++ch) { shelf[ch].setHighShelf (1681.97f, 0.7072f, 4.0f, s); hp[ch].setHighPass (38.135f, 0.5003f, s); }
        blockLen = juce::jmax (1, (int) (0.1 * s));
        history.assign (36000, 0.0f);   // one hour of 100 ms blocks
        reset();
    }
    void reset() override
    {
        for (auto& b : shelf) b.reset(); for (auto& b : hp) b.reset();
        acc = 0.0; count = 0; recent.fill (0.0f); recentW = 0; nHist = 0; lastReset = -1;
        taps->lufsM.store (-100.0f); taps->lufsS.store (-100.0f); taps->lufsI.store (-100.0f);
    }
    static float lufs (double ms) { return ms > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (ms)) : -100.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int rs = ci (c, 2);
        if (lastReset >= 0 && rs != lastReset) { nHist = 0; taps->lufsI.store (-100.0f); }
        lastReset = rs;
        for (int i = 0; i < n; ++i)
        {
            const float l = hp[0].process (shelf[0].process (L[i])), r = hp[1].process (shelf[1].process (R[i]));
            acc += (double) l * l + (double) r * r;
            if (++count >= blockLen) block();
        }
    }
    void block()
    {
        recent[(size_t) recentW] = (float) (acc / count);
        recentW = (recentW + 1) % (int) recent.size();
        acc = 0.0; count = 0;
        auto meanOf = [this] (int blocks)
        {
            double s = 0.0;
            for (int k = 1; k <= blocks; ++k) s += recent[(size_t) ((recentW - k + (int) recent.size()) % (int) recent.size())];
            return s / blocks;
        };
        const double mom = meanOf (4);
        taps->lufsM.store (lufs (mom));
        taps->lufsS.store (lufs (meanOf (30)));
        // integrated: 400 ms blocks (75% overlap), absolute gate -70, relative gate -10 LU
        if (nHist < (int) history.size() && lufs (mom) > -70.0f) history[(size_t) nHist++] = (float) mom;
        if (nHist > 0)
        {
            double s = 0.0; for (int k = 0; k < nHist; ++k) s += history[(size_t) k];
            const float rel = lufs (s / nHist) - 10.0f;
            double s2 = 0.0; int m = 0;
            for (int k = 0; k < nHist; ++k) if (lufs (history[(size_t) k]) > rel) { s2 += history[(size_t) k]; ++m; }
            taps->lufsI.store (m > 0 ? lufs (s2 / m) : -100.0f);
        }
    }
private:
    Biquad shelf[2], hp[2];
    double acc = 0.0;
    int count = 0, blockLen = 4800, recentW = 0, nHist = 0, lastReset = -1;
    std::array<float, 30> recent {};
    std::vector<float> history;
};

// ------------------------------------------------------------------ Stereo scope
// Gain, Persist, Auto Gain | Gonio / Liss
class StereoScopeFx : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { taps->corr.store (0.0f); }
    void process (float* L, float* R, int n, const FxContext&) override
    {
        float lr = 0.0f, ll = 0.0f, rr = 0.0f;
        for (int i = 0; i < n; ++i) { lr += L[i] * R[i]; ll += L[i] * L[i]; rr += R[i] * R[i]; }
        const float d = std::sqrt (ll * rr);
        if (d > 1.0e-10f)
        {
            const float prev = taps->corr.load();
            taps->corr.store (prev + (lr / d - prev) * 0.05f);
        }
    }
};

// ------------------------------------------------------------------ Pitch analysis
// Ref A, Smooth, Sens | Note / Freq
class PitchAnalysis : public FxImpl
{
public:
    void prepare (double s) override { pt.prepare (s); reset(); }
    void reset() override { pt.reset(); midi = 0.0f; taps->pitchHz.store (0.0f); taps->pitchConf.store (0.0f); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        for (int i = 0; i < n; ++i) pt.push (0.5f * (L[i] + R[i]));
        const float need = 0.9f - c.p[2] * 0.6f;
        if (pt.periodSamples > 0.0f && pt.confidence >= need)
        {
            const float m = freqToMidi (pt.hz());
            const float coef = 1.0f - std::exp (-(float) n / (juce::jmax (1.0f, c.p[1]) * 0.001f * (float) sr));
            midi = (midi <= 0.0f || std::abs (m - midi) > 1.0f) ? m : midi + (m - midi) * coef;
            taps->pitchHz.store (midiToFreq (midi));
            taps->pitchConf.store (pt.confidence);
        }
        else
        {
            taps->pitchConf.store (taps->pitchConf.load() * 0.98f);
            if (taps->pitchConf.load() < need * 0.5f) taps->pitchHz.store (0.0f);
        }
    }
private:
    PitchTracker pt;
    float midi = 0.0f;
};

// ------------------------------------------------------------------ Chord detector
// Sens, Smooth, Low, High, Names | Chord / Notes
// Chromagram from an 8192-point FFT (Low..High Hz), smoothed, matched against
// chord templates for every root (cosine similarity).
// Taps: spec[0..11] chroma 0..1; m[0] root (-1 none), m[1] quality, m[2] confidence
class ChordDetector : public FxImpl
{
public:
    static constexpr int kOrder = 13, kN = 1 << kOrder, kHop = 2048;
    void prepare (double) override
    {
        ring.assign (kN, 0.0f); work.assign (kN * 2, 0.0f); win.resize (kN);
        peaks.reserve (256);
        for (int i = 0; i < kN; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) kN);
        reset();
    }
    void reset() override
    {
        std::fill (ring.begin(), ring.end(), 0.0f); w = hop = 0;
        for (auto& v : chroma) v = 0.0f;
        for (auto& v : bass) v = 0.0f;
        taps->set (0, -1.0f); taps->set (1, 0.0f); taps->set (2, 0.0f);
        for (int k = 0; k < 12; ++k) taps->spec[(size_t) k].store (0.0f);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) w] = 0.5f * (L[i] + R[i]);
            w = (w + 1) & (kN - 1);
            if (++hop >= kHop) { hop = 0; analyse (c); }
        }
    }
    void analyse (const FxContext& c)
    {
        for (int i = 0; i < kN; ++i) work[(size_t) i] = ring[(size_t) ((w + i) & (kN - 1))] * win[(size_t) i];
        std::fill (work.begin() + kN, work.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        const float binHz = (float) sr / (float) kN;
        float cur[12] {};
        float energy = 0.0f;
        const int k0 = juce::jmax (1, (int) (c.p[2] / binHz)), k1 = juce::jmin (kN / 2 - 1, (int) (c.p[3] / binHz));
        float peakMax = 0.0f;
        for (int k = k0; k <= k1; ++k) peakMax = juce::jmax (peakMax, work[(size_t) k]);
        // spectral peaks (local maxima above -50 dB of the loudest), with their
        // pitch class and how well they're tuned
        peaks.clear();
        for (int k = k0; k <= k1; ++k)
        {
            const float m = work[(size_t) k];
            energy += m * m;
            if (m < work[(size_t) k - 1] || m < work[(size_t) k + 1] || m < peakMax * 0.003f) continue;   // only local peaks: suppresses window leakage
            // parabolic interpolation for the true peak frequency
            const float a0 = work[(size_t) k - 1], a2 = work[(size_t) k + 1];
            const float den = a0 - 2.0f * m + a2;
            const float off = std::abs (den) > 1.0e-12f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a0 - a2) / den) : 0.0f;
            const float hz = ((float) k + off) * binHz;
            const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
            const float pc = midi - 12.0f * std::floor (midi / 12.0f);
            const float dev = std::abs (pc - std::round (pc));
            peaks.push_back ({ hz, m * m, 1.0f - dev, (int) std::floor (pc + 0.5f) % 12 });
            if (peaks.size() >= 256) break;
        }
        // overtones aren't notes: a peak at the 3rd / 5th / 6th / 7th harmonic of
        // a stronger lower peak (which would add a fifth / major third / flat
        // seventh that nobody played) counts for little. Octaves are harmless.
        int bassPcNow = -1;
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            auto& p = peaks[i];
            for (size_t j = 0; j < i; ++j)
            {
                const auto& f0 = peaks[j];
                if (f0.e < p.e * 0.5f) continue;
                const float r = p.hz / f0.hz;
                const int h = (int) std::lround (r);
                if ((h == 3 || h == 5 || h == 6 || h == 7) && std::abs (r - (float) h) < 0.02f * (float) h) { p.w *= 0.15f; break; }
            }
            cur[p.pc] += p.e * p.w;   // well-tuned peaks count more
            if (bassPcNow < 0 && p.e > 0.06f * peakMax * peakMax && p.w > 0.5f) bassPcNow = p.pc;   // lowest strong note
        }
        float mx = 1.0e-12f;
        for (float v : cur) mx = juce::jmax (mx, v);
        const float hopSec = (float) kHop / (float) sr;
        const float coef = 1.0f - std::exp (-hopSec / juce::jmax (0.02f, c.p[1] * 0.001f));
        float cmax = 1.0e-9f;
        for (int k = 0; k < 12; ++k) { chroma[k] += (cur[k] / mx - chroma[k]) * coef; cmax = juce::jmax (cmax, chroma[k]); }
        for (int k = 0; k < 12; ++k) taps->spec[(size_t) k].store (chroma[k] / cmax);
        for (int k = 0; k < 12; ++k) bass[k] += ((k == bassPcNow ? 1.0f : 0.0f) - bass[k]) * coef;
        int bassPc = -1; float bassBest = 0.3f;
        for (int k = 0; k < 12; ++k) if (bass[k] > bassBest) { bassBest = bass[k]; bassPc = k; }

        const float levelDb = 10.0f * std::log10 (juce::jmax (1.0e-12f, energy / (float) kN)) - 40.0f;
        const float gate = -70.0f + 50.0f * (1.0f - c.p[0]);
        float best = 0.0f; int bestRoot = -1, bestQ = 0;
        float norm = 0.0f;
        for (float v : chroma) norm += v * v;
        norm = std::sqrt (juce::jmax (1.0e-9f, norm));
        for (int root = 0; root < 12; ++root)
            for (int q = 0; q < kNumChordQualities; ++q)
            {
                const auto& t = kChordQualities[q];
                float dot = 0.0f; int cnt = 0;
                for (int j = 0; j < 6; ++j)
                    if (t.iv[j] >= 0) { dot += chroma[(root + t.iv[j]) % 12] * (j == 0 ? 1.15f : 1.0f); ++cnt; }
                // cosine similarity; each note past a triad costs a little, so
                // extended chords win only when their extra notes are really there
                // and the bass note is most likely the root (harmonics of the
                // lower notes otherwise pull the root to the fifth)
                const float score = dot / (norm * std::sqrt ((float) cnt)) - 0.02f * (float) (cnt - 3)
                                    + (root == bassPc ? 0.08f : 0.0f);
                if (score > best) { best = score; bestRoot = root; bestQ = q; }
            }
        const bool ok = levelDb > gate && best > 0.55f;
        taps->set (0, ok ? (float) bestRoot : -1.0f);
        taps->set (1, (float) bestQ);
        taps->set (2, ok ? best : 0.0f);
    }
private:
    juce::dsp::FFT fft { kOrder };
    std::vector<float> ring, work, win;
    int w = 0, hop = 0;
    float chroma[12] {};
    struct Peak { float hz, e, w; int pc; };
    std::vector<Peak> peaks;    // reserved in prepare: no allocation while analysing
    float bass[12] {};          // smoothed "lowest strong note" per pitch class
};

} // namespace

std::unique_ptr<FxImpl> makeAnalysisFx (const juce::String& n)
{
    if (n == "Spectrum Analyzer") return std::make_unique<Spectrum>();
    if (n == "Oscilloscope")      return std::make_unique<Scope>();
    if (n == "Loudness Meter")    return std::make_unique<Loudness>();
    if (n == "Stereo Scope")      return std::make_unique<StereoScopeFx>();
    if (n == "Pitch Analysis")    return std::make_unique<PitchAnalysis>();
    if (n == "Chord Detector")    return std::make_unique<ChordDetector>();
    return nullptr;
}

// ------------------------------------------------------------------ factory
std::unique_ptr<FxImpl> makeFxImpl (int effectId)
{
    const auto* d = defForId (effectId);
    jassert (d != nullptr);
    const juce::String name (d != nullptr ? d->name : "");
    using Maker = std::unique_ptr<FxImpl> (*) (const juce::String&);
    for (Maker m : { makeAnalysisFx, makeDynamicsFx, makeEqVolumeFx, makeFilterFx, makeModulationFx,
                     makeStereoPitchFx, makeDriveFx, makeSpaceFx, makeSpectralTimeFx })
        if (auto fx = m (name)) return fx;
    jassertfalse;   // every registry entry must have an implementation
    return std::make_unique<Scope>();
}

} // namespace efx
