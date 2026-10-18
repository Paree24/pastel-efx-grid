#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <cmath>
#include <complex>
#include <algorithm>

// ============================================================
// Pastel EFX Grid — shared DSP primitives.
// Real-time safe: prepare() allocates, process()/tick() never do.
// ============================================================

namespace efx
{

constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) { return 20.0f * std::log10 (juce::jmax (1.0e-9f, g)); }
inline float midiToFreq (float m) { return 440.0f * std::pow (2.0f, (m - 69.0f) / 12.0f); }
inline float freqToMidi (float f) { return 69.0f + 12.0f * std::log2 (juce::jmax (1.0e-6f, f) / 440.0f); }
inline float semisToRatio (float s) { return std::pow (2.0f, s / 12.0f); }
inline float clamp01 (float x) { return juce::jlimit (0.0f, 1.0f, x); }
inline float lerp (float a, float b, float t) { return a + (b - a) * t; }
inline float softClip (float x) { return x / (1.0f + std::abs (x)); }
inline float fastTanh (float x)
{
    x = juce::jlimit (-4.5f, 4.5f, x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// coefficient for a one-pole smoother reaching ~63% in `ms`
inline float msCoef (float ms, double sr) { return 1.0f - std::exp (-1.0f / (juce::jmax (0.001f, ms) * 0.001f * (float) sr)); }
inline float sanitize (float x) { return std::isfinite (x) ? x : 0.0f; }

// ---------------------------------------------------------------
// deterministic xorshift RNG (audio-safe, no locks)
struct Rng
{
    juce::uint32 s = 0x1234567u;
    void seed (juce::uint32 v) { s = v | 1u; }
    inline juce::uint32 nextu() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float uni() { return (float) (nextu() & 0xFFFFFF) / (float) 0xFFFFFF; }   // 0..1
    inline float bipolar() { return uni() * 2.0f - 1.0f; }
};

// ---------------------------------------------------------------
// one-pole lowpass / highpass
struct OnePole
{
    float z = 0.0f, a = 0.5f;
    void setCutoff (float hz, double sr)
    {
        a = 1.0f - std::exp (-kTwoPi * juce::jlimit (1.0f, (float) sr * 0.49f, hz) / (float) sr);
    }
    void reset() { z = 0.0f; }
    inline float lp (float x) { z += a * (x - z); return z; }
    inline float hp (float x) { z += a * (x - z); return x - z; }
};

struct DcBlocker
{
    float x1 = 0.0f, y1 = 0.0f;
    void reset() { x1 = y1 = 0.0f; }
    inline float process (float x, float r = 0.9995f) { float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

// attack/release envelope follower on |x|
struct EnvFollower
{
    float env = 0.0f, a = 0.1f, r = 0.001f;
    void set (float attackMs, float releaseMs, double sr) { a = msCoef (attackMs, sr); r = msCoef (releaseMs, sr); }
    void reset() { env = 0.0f; }
    inline float process (float x) { x = std::abs (x); env += (x > env ? a : r) * (x - env); return env; }
};

// ---------------------------------------------------------------
// LFO: 0 sine, 1 tri, 2 saw (up), 3 ramp (down), 4 square, 5 sample&hold. Output -1..1
struct Lfo
{
    Rng rng;
    double phase = 0.0;
    float held = 0.0f;
    void reset (double ph = 0.0) { phase = ph; held = 0.0f; }
    static inline float shape (int wave, double ph, float held)
    {
        switch (wave)
        {
            case 0: return std::sin ((float) (juce::MathConstants<double>::twoPi * ph));
            case 1: return (float) (1.0 - 4.0 * std::abs (ph - 0.5));
            case 2: return (float) (2.0 * ph - 1.0);
            case 3: return (float) (1.0 - 2.0 * ph);
            case 4: return ph < 0.5 ? 1.0f : -1.0f;
            default: return held;
        }
    }
    inline float valueAt (int wave, double offset) const
    {
        double ph = phase + offset; ph -= std::floor (ph);
        return shape (wave, ph, held);
    }
    inline void advance (double inc)
    {
        phase += inc;
        if (phase >= 1.0) { phase -= std::floor (phase); held = rng.bipolar(); }
    }
};

// ---------------------------------------------------------------
// Drawable curve stored in a slot's shape data:
//   d[0] = point count (2..16), then per point (x, y, curve)
// x rises 0..1 (first point x = 0, last x = 1); `curve` (-1..1) bends the
// segment that starts at that point (0 = straight).
struct CurvePoints
{
    static constexpr int kMaxPts = 16;
    static constexpr int kSize = 1 + kMaxPts * 3;
    static int count (const float* d) { return juce::jlimit (2, kMaxPts, (int) std::lround (d[0])); }
    static float bend (float t, float c)
    {
        if (std::abs (c) < 1.0e-3f) return t;
        return c > 0.0f ? std::pow (t, 1.0f + c * 6.0f) : 1.0f - std::pow (1.0f - t, 1.0f - c * 6.0f);
    }
    static float eval (const float* d, float x)
    {
        const int n = count (d);
        x = clamp01 (x);
        for (int i = 0; i < n - 1; ++i)
        {
            const float x0 = d[1 + i * 3], x1 = d[1 + (i + 1) * 3];
            if (x <= x1 || i == n - 2)
            {
                const float t = x1 > x0 ? clamp01 ((x - x0) / (x1 - x0)) : 1.0f;
                return lerp (d[2 + i * 3], d[2 + (i + 1) * 3], bend (t, d[3 + i * 3]));
            }
        }
        return d[2];
    }
    static void set (float* d, std::initializer_list<std::array<float, 3>> pts)
    {
        int i = 0;
        for (auto& p : pts) { if (i >= kMaxPts) break; d[1 + i * 3] = p[0]; d[2 + i * 3] = p[1]; d[3 + i * 3] = p[2]; ++i; }
        d[0] = (float) i;
    }
};

// ---------------------------------------------------------------
// LFO with selectable shapes, morph toward the next shape, a drawn custom
// shape and freeze. Output -1..1.
namespace LfoShapes
{
    constexpr int kNum = 12, kCustom = 11;
    inline float value (int shape, double ph, const float* custom, float held, float next)
    {
        ph -= std::floor (ph);
        const float p = (float) ph;
        switch (shape)
        {
            case 0: return std::sin (kTwoPi * p);
            case 1: return p < 0.25f ? 4.0f * p : (p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f);
            case 2: return 2.0f * p - 1.0f;
            case 3: return 1.0f - 2.0f * p;
            case 4: return p < 0.5f ? 1.0f : -1.0f;
            case 5: return p < 0.25f ? 1.0f : -1.0f;
            case 6: return held;
            case 7: return lerp (held, next, 0.5f - 0.5f * std::cos (kPi * p));
            case 8: return 2.0f * (std::exp (4.0f * p) - 1.0f) / (std::exp (4.0f) - 1.0f) - 1.0f;
            case 9: return 1.0f - 2.0f * (std::exp (4.0f * (1.0f - p)) - 1.0f) / (std::exp (4.0f) - 1.0f);
            case 10: return 2.0f * std::abs (std::sin (kTwoPi * p)) - 1.0f;
            default: return custom != nullptr ? CurvePoints::eval (custom, p) : std::sin (kTwoPi * p);
        }
    }
    // default custom shape: a sine-like curve
    inline void defaultCustom (float* d)
    {
        CurvePoints::set (d, { { 0.0f, 0.0f, -0.35f }, { 0.25f, 1.0f, 0.35f }, { 0.5f, 0.0f, -0.35f }, { 0.75f, -1.0f, 0.35f }, { 1.0f, 0.0f, 0.0f } });
    }
}

struct ShapeLfo
{
    double phase = 0.0;
    float held = 0.0f, next = 0.0f;
    Rng rng;
    void reset (double ph = 0.0) { phase = ph; held = 0.0f; next = rng.bipolar(); }
    // shape, morph (0..1 toward the next shape in the list), custom points
    float value (int shape, float morph, const float* custom, double offset = 0.0) const
    {
        const double ph = phase + offset;
        const float a = LfoShapes::value (shape, ph, custom, held, next);
        if (morph <= 0.001f) return a;
        const float b = LfoShapes::value ((shape + 1) % LfoShapes::kNum, ph, custom, held, next);
        return lerp (a, b, morph);
    }
    void advance (double inc)
    {
        phase += inc;
        if (phase >= 1.0) { phase -= std::floor (phase); held = next; next = rng.bipolar(); }
    }
};

// ---------------------------------------------------------------
// RBJ biquad
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;   // transposed direct form II
    void reset() { z1 = z2 = 0; }
    inline float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void copyCoeffs (const Biquad& o) { b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2; }
    void setCoeffs (double bb0, double bb1, double bb2, double aa0, double aa1, double aa2)
    {
        const double inv = 1.0 / aa0;
        b0 = (float) (bb0 * inv); b1 = (float) (bb1 * inv); b2 = (float) (bb2 * inv);
        a1 = (float) (aa1 * inv); a2 = (float) (aa2 * inv);
    }
    static double w0 (float f, double sr) { return 2.0 * juce::MathConstants<double>::pi * juce::jlimit (5.0, sr * 0.49, (double) f) / sr; }
    void setPeak (float f, float q, float gdb, double sr)
    {
        const double A = std::pow (10.0, gdb / 40.0), w = w0 (f, sr);
        const double c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        setCoeffs (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void setLowShelf (float f, float q, float gdb, double sr)
    {
        const double A = std::pow (10.0, gdb / 40.0), w = w0 (f, sr);
        const double c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q)), tsa = 2 * std::sqrt (A) * al;
        setCoeffs (A * ((A + 1) - (A - 1) * c + tsa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - tsa),
                   (A + 1) + (A - 1) * c + tsa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - tsa);
    }
    void setHighShelf (float f, float q, float gdb, double sr)
    {
        const double A = std::pow (10.0, gdb / 40.0), w = w0 (f, sr);
        const double c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q)), tsa = 2 * std::sqrt (A) * al;
        setCoeffs (A * ((A + 1) + (A - 1) * c + tsa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - tsa),
                   (A + 1) - (A - 1) * c + tsa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - tsa);
    }
    void setLowPass (float f, float q, double sr)
    {
        const double w = w0 (f, sr), c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        setCoeffs ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void setHighPass (float f, float q, double sr)
    {
        const double w = w0 (f, sr), c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        setCoeffs ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void setNotch (float f, float q, double sr)
    {
        const double w = w0 (f, sr), c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        setCoeffs (1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }
    void setBandPass (float f, float q, double sr)   // 0 dB peak gain
    {
        const double w = w0 (f, sr), c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        setCoeffs (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
    void setAllPass (float f, float q, double sr)
    {
        const double w = w0 (f, sr), c = std::cos (w), al = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        setCoeffs (1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
    }
    // magnitude response (dB) at frequency f — used by the visualisers
    float magnitudeDb (float f, double sr) const
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        const std::complex<double> z1c = std::polar (1.0, -w), z2c = std::polar (1.0, -2.0 * w);
        const auto num = (double) b0 + (double) b1 * z1c + (double) b2 * z2c;
        const auto den = 1.0 + (double) a1 * z1c + (double) a2 * z2c;
        return (float) (20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (num) / juce::jmax (1.0e-12, std::abs (den)))));
    }
};

// ---------------------------------------------------------------
// TPT state-variable filter (Zavalishin): LP/BP/HP from one update
struct Svf
{
    double sr = 44100.0, ic1 = 0, ic2 = 0;
    float cCut = -1.0f, cQ = -1.0f;        // coefficient cache (cutoff / Q unchanged = no tan())
    double cK = 1.0, cG = 0.0, cA1 = 1.0;
    void prepare (double s) { sr = s; cCut = -1.0f; reset(); }
    void reset() { ic1 = ic2 = 0; }
    static float resToQ (float res01) { return 0.5f + res01 * res01 * 39.5f; }
    inline void processMulti (float x, float cutoff, float res01, float& lp, float& bp, float& hp)
    {
        processQ (x, cutoff, resToQ (res01), lp, bp, hp);
    }
    inline void processQ (float x, float cutoff, float q, float& lp, float& bp, float& hp)
    {
        if (cutoff != cCut || q != cQ)
        {
            cCut = cutoff; cQ = q;
            const float fc = juce::jlimit (10.0f, (float) (sr * 0.49), cutoff);
            cK = 1.0 / juce::jmax (0.05f, q);
            cG = std::tan (juce::MathConstants<double>::pi * fc / sr);
            cA1 = 1.0 / (1.0 + cG * (cG + cK));
        }
        const double k = cK, g = cG, a1 = cA1;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a1 * g * v3;
        const double v2 = ic2 + g * v1;
        ic1 = 2.0 * v1 - ic1; ic2 = 2.0 * v2 - ic2;
        if (! std::isfinite (ic1) || ! std::isfinite (ic2)) ic1 = ic2 = 0;
        lp = (float) v2; bp = (float) v1; hp = (float) (x - k * v1 - v2);
    }
    // Saturating variant: the band-pass integrator is soft-limited, which
    // bounds the resonant peak (no runaway / clipping at high Q) and gives the
    // squelchy, level-dependent resonance of analogue filters.
    inline void processNL (float x, float cutoff, float q, float& lp, float& bp, float& hp)
    {
        if (cutoff != cCut || q != cQ)
        {
            cCut = cutoff; cQ = q;
            const float fc = juce::jlimit (10.0f, (float) (sr * 0.49), cutoff);
            cK = 1.0 / juce::jmax (0.05f, q);
            cG = std::tan (juce::MathConstants<double>::pi * fc / sr);
            cA1 = 1.0 / (1.0 + cG * (cG + cK));
        }
        const double k = cK, g = cG, a1 = cA1;
        const double v3 = x - ic2;
        double v1 = a1 * ic1 + a1 * g * v3;
        v1 = 1.2 * std::tanh (v1 / 1.2);
        const double v2 = ic2 + g * v1;
        ic1 = 2.0 * v1 - ic1; ic2 = 2.0 * v2 - ic2;
        if (! std::isfinite (ic1) || ! std::isfinite (ic2)) ic1 = ic2 = 0;
        lp = (float) v2; bp = (float) v1; hp = (float) (x - k * v1 - v2);
    }
    inline float processNLMode (float x, float cutoff, float res01, int mode)
    {
        float lp, bp, hp;
        const float q = resToQ (res01);
        processNL (x, cutoff, q, lp, bp, hp);
        switch (mode)
        {
            case 0: return lp;
            case 1: return hp;
            case 2: return bp / std::sqrt (q);
            default: return lp + hp;
        }
    }
    // 0 LP, 1 HP, 2 BP (unity peak), 3 notch
    inline float process (float x, float cutoff, float res01, int mode)
    {
        float lp, bp, hp;
        const float q = resToQ (res01);
        processQ (x, cutoff, q, lp, bp, hp);
        switch (mode)
        {
            case 0: return lp;
            case 1: return hp;
            case 2: return bp / q;
            default: return lp + hp;
        }
    }
    // analytic magnitude (dB) of the same response, for drawing
    static float responseDb (int mode, float f, float cutoff, float res01, int order = 1)
    {
        const double q = resToQ (res01), w = (double) f / juce::jmax (1.0f, cutoff);
        const std::complex<double> s (0.0, w);
        const auto den = s * s + s / q + 1.0;
        std::complex<double> num;
        switch (mode)
        {
            case 0: num = 1.0; break;
            case 1: num = s * s; break;
            case 2: num = s / q; break;
            default: num = s * s + 1.0; break;
        }
        const double m = std::abs (num / den);
        return (float) (20.0 * order * std::log10 (juce::jmax (1.0e-9, m)));
    }
};

// ---------------------------------------------------------------
// zero-delay-feedback 4-pole transistor ladder with input saturation
struct Ladder
{
    double sr = 44100.0;
    float s[4] = { 0, 0, 0, 0 };
    void prepare (double r) { sr = r; reset(); }
    void reset() { s[0] = s[1] = s[2] = s[3] = 0.0f; }
    // res01 0..1 (self-oscillates near 1); drive >= 1; returns 24 dB (or 12 dB) output
    inline float process (float x, float cutoff, float res01, float drive, bool twoPole, float bassComp = 0.0f)
    {
        cutoff = juce::jlimit (10.0f, (float) (sr * 0.45), cutoff);
        const float g = std::tan (kPi * cutoff / (float) sr);
        const float G = g / (1.0f + g);
        const float k = res01 * 4.1f;
        const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        const float S = G3 * (1.0f - G) * s[0] + G2 * (1.0f - G) * s[1] + G * (1.0f - G) * s[2] + (1.0f - G) * s[3];
        float u = (x * (1.0f + bassComp * k * 0.5f) - k * S) / (1.0f + k * G4);
        u = fastTanh (u * drive) / juce::jmax (0.5f, std::sqrt (drive));
        float y = u;
        float taps[4];
        for (int i = 0; i < 4; ++i)
        {
            const float v = (y - s[i]) * G;
            y = v + s[i];
            s[i] = y + v;
            taps[i] = y;
        }
        for (auto& v : s) if (! std::isfinite (v)) v = 0.0f;
        return twoPole ? taps[1] : taps[3];
    }
};

// ---------------------------------------------------------------
// 4th-order Linkwitz-Riley crossover (LP + HP sum to an all-pass)
struct LR4
{
    Biquad lp[2], hp[2];
    void set (float f, double sr)
    {
        for (int i = 0; i < 2; ++i) { lp[i].setLowPass (f, 0.70710678f, sr); hp[i].setHighPass (f, 0.70710678f, sr); }
    }
    void reset() { for (auto& b : lp) b.reset(); for (auto& b : hp) b.reset(); }
    inline void split (float x, float& lo, float& hi)
    {
        lo = lp[1].process (lp[0].process (x));
        hi = hp[1].process (hp[0].process (x));   // lo + hi = all-pass (in phase)
    }
};

// ---------------------------------------------------------------
// N-band perfectly-reconstructing splitter: band i = lp_i(rest) where rest is
// whatever the lower bands did not take. The bands always sum back to the
// input exactly (zero latency), at the cost of gentler band slopes.
template <int MaxBands>
struct BandSplitter
{
    Svf lp[MaxBands];
    float freq[MaxBands] = {};
    int bands = MaxBands;
    double sr = 44100.0;
    void prepare (double s, int numBands, float fLo, float fHi)
    {
        sr = s; bands = juce::jlimit (2, MaxBands, numBands);
        for (auto& f : lp) f.prepare (s);
        setRange (fLo, fHi);
    }
    void setRange (float fLo, float fHi)
    {
        // band edges (bands-1 crossovers) spread log between fLo and fHi
        for (int i = 0; i < bands - 1; ++i)
            freq[i] = fLo * std::pow (fHi / fLo, (float) i / (float) juce::jmax (1, bands - 2));
    }
    void reset() { for (auto& f : lp) f.reset(); }
    // centre frequency of band b (for display / band weighting)
    float centre (int b) const
    {
        if (b <= 0) return freq[0] * 0.5f;
        if (b >= bands - 1) return freq[bands - 2] * 1.6f;
        return std::sqrt (freq[b - 1] * freq[b]);
    }
    inline void split (float x, float* out)
    {
        float rest = x;
        for (int i = 0; i < bands - 1; ++i)
        {
            float l, b, h;
            lp[i].processQ (rest, freq[i], 0.5f, l, b, h);   // critically damped 2-pole
            out[i] = l;
            rest -= l;
        }
        out[bands - 1] = rest;
    }
};

// ---------------------------------------------------------------
// interpolated mono delay line
struct DelayLine
{
    std::vector<float> buf;
    int len = 0, w = 0;
    void prepare (double sr, float maxSec) { len = juce::jmax (8, (int) (sr * maxSec) + 8); buf.assign ((size_t) len, 0.0f); w = 0; }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    inline void write (float x) { buf[(size_t) w] = x; if (++w >= len) w = 0; }
    inline float tap (int d) const { int i = w - d; while (i < 0) i += len; return buf[(size_t) i]; }
    // delay >= 1 sample (fractional, linear)
    inline float read (float d) const
    {
        d = juce::jlimit (1.0f, (float) (len - 4), d);
        const int di = (int) d; const float fr = d - (float) di;
        return tap (di) * (1.0f - fr) + tap (di + 1) * fr;
    }
    // cubic (Hermite) read — for modulated delays
    inline float readCubic (float d) const
    {
        d = juce::jlimit (2.0f, (float) (len - 4), d);
        const int di = (int) d; const float t = d - (float) di;
        const float y0 = tap (di - 1), y1 = tap (di), y2 = tap (di + 1), y3 = tap (di + 2);
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
    int maxDelay() const { return len - 4; }
};

// Schroeder all-pass on a delay line
struct AllpassDelay
{
    DelayLine d;
    void prepare (double sr, float maxSec) { d.prepare (sr, maxSec); }
    void reset() { d.reset(); }
    inline float process (float x, float delaySamples, float g)
    {
        const float z = d.readCubic (delaySamples);
        const float v = x + g * z;
        d.write (v);
        return z - g * v;
    }
};

// first-order all-pass (phaser / disperser stage)
struct Allpass1
{
    float z = 0.0f;
    void reset() { z = 0.0f; }
    static float coef (float f, double sr) { const float t = std::tan (kPi * juce::jlimit (5.0f, (float) sr * 0.49f, f) / (float) sr); return (t - 1.0f) / (t + 1.0f); }
    inline float process (float x, float a) { const float y = a * x + z; z = x - a * y; return y; }
};

// ---------------------------------------------------------------
// Hilbert pair (Olli Niemitalo's 2 x 4 second-order all-pass sections):
// outputs two signals 90 degrees apart over ~20 Hz .. 0.47 fs.
struct Hilbert
{
    struct Ap2 { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    Ap2 a[4], b[4];
    float dly = 0.0f;
    void reset() { for (auto& s : a) s = {}; for (auto& s : b) s = {}; dly = 0.0f; }
    static inline float stage (Ap2& s, float x, float c)
    {
        const float y = c * (x + s.y2) - s.x2;
        s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
        return y;
    }
    inline void process (float x, float& re, float& im)
    {
        static constexpr float ca[4] = { 0.6923878f * 0.6923878f, 0.9360654322959f * 0.9360654322959f,
                                         0.9882295226860f * 0.9882295226860f, 0.9987488452737f * 0.9987488452737f };
        static constexpr float cb[4] = { 0.4021921162426f * 0.4021921162426f, 0.8561710882420f * 0.8561710882420f,
                                         0.9722909545651f * 0.9722909545651f, 0.9952884791278f * 0.9952884791278f };
        float u = x, v = x;
        for (int i = 0; i < 4; ++i) u = stage (a[i], u, ca[i]);
        for (int i = 0; i < 4; ++i) v = stage (b[i], v, cb[i]);
        re = dly; dly = u;   // one-sample delay on path A
        im = v;
    }
};

// single-sideband frequency shifter
struct FreqShifter
{
    Hilbert h;
    double phase = 0.0;
    void reset() { h.reset(); phase = 0.0; }
    inline float process (float x, float shiftHz, double sr)
    {
        float re, im;
        h.process (x, re, im);
        phase += (double) shiftHz / sr;
        phase -= std::floor (phase);
        const float ph = (float) (phase * juce::MathConstants<double>::twoPi);
        return re * std::cos (ph) + im * std::sin (ph);   // upper sideband for shiftHz > 0
    }
};

// ---------------------------------------------------------------
// two-tap crossfaded delay-line pitch shifter (polyphonic, ~window latency)
struct PitchShifter
{
    std::vector<float> buf;
    int len = 0, w = 0;
    double phase = 0.0;   // 0..1 sweep through the window
    void prepare (double sr) { len = (int) (sr * 0.5) + 8; buf.assign ((size_t) len, 0.0f); reset(); }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; phase = 0.0; }
    inline float tap (float d) const
    {
        d = juce::jlimit (1.0f, (float) (len - 3), d);
        int i0 = w - 1 - (int) d; while (i0 < 0) i0 += len;
        int i1 = i0 - 1; if (i1 < 0) i1 += len;
        const float fr = d - std::floor (d);
        return buf[(size_t) i0] * (1.0f - fr) + buf[(size_t) i1] * fr;
    }
    inline float process (float x, float ratio, float windowSamples)
    {
        buf[(size_t) w] = x; if (++w >= len) w = 0;
        const float win = juce::jlimit (64.0f, (float) (len / 2), windowSamples);
        phase += (1.0 - (double) ratio) / win;
        phase -= std::floor (phase);
        const float pA = (float) phase, pB = (float) (phase + 0.5 - std::floor (phase + 0.5));
        const float gA = std::sin (kPi * pA), gB = std::sin (kPi * pB);
        return tap (pA * win) * gA + tap (pB * win) * gB;
    }
};

// ---------------------------------------------------------------
// YIN pitch tracker on a 4x decimated stream (~70 Hz .. 1 kHz).
struct PitchTracker
{
    static constexpr int kDec = 4, kWin = 512, kMaxLag = 180;
    std::vector<float> hist;   // decimated history
    int hw = 0, decCount = 0, hop = 0;
    float lp1 = 0, lp2 = 0;
    float periodSamples = 0.0f;   // in input samples (0 = unvoiced)
    float confidence = 0.0f;
    double sr = 44100.0;
    float lpA = 0.3f;
    std::array<float, kMaxLag + 2> dbuf {};
    void prepare (double s) { sr = s; hist.assign ((size_t) (kWin + kMaxLag + 4), 0.0f); lpA = 1.0f - std::exp (-kTwoPi * 2500.0f / (float) s); reset(); }
    void reset() { std::fill (hist.begin(), hist.end(), 0.0f); hw = decCount = hop = 0; lp1 = lp2 = 0; periodSamples = 0; confidence = 0; }
    float hz() const { return periodSamples > 0.0f ? (float) sr / periodSamples : 0.0f; }
    inline void push (float x)
    {
        lp1 += lpA * (x - lp1); lp2 += lpA * (lp1 - lp2);
        if (++decCount < kDec) return;
        decCount = 0;
        hist[(size_t) hw] = lp2;
        if (++hw >= (int) hist.size()) hw = 0;
        if (++hop >= 128) { hop = 0; analyse(); }
    }
    std::array<float, kWin + kMaxLag + 4> lin {};
    void analyse()
    {
        const int n = (int) hist.size();
        for (int i = 0; i < n; ++i) { int k = hw + i; if (k >= n) k -= n; lin[(size_t) i] = hist[(size_t) k]; }   // oldest..newest
        auto at = [this] (int i) { return lin[(size_t) i]; };
        const double dsr = sr / kDec;
        const int minLag = juce::jmax (2, (int) (dsr / 1000.0));
        const int maxLag = juce::jmin (kMaxLag, (int) (dsr / 65.0));
        float energy = 0.0f;
        for (int j = 0; j < kWin; ++j) energy += at (j) * at (j);
        if (energy < 1.0e-5f) { confidence *= 0.5f; if (confidence < 0.2f) periodSamples = 0.0f; return; }
        dbuf[0] = 1.0f;
        float run = 0.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            float d = 0.0f;
            for (int j = 0; j < kWin; ++j) { const float df = at (j) - at (j + tau); d += df * df; }
            run += d;
            dbuf[(size_t) tau] = run > 0.0f ? d * (float) tau / run : 1.0f;
        }
        int best = -1;
        for (int tau = minLag; tau < maxLag; ++tau)
            if (dbuf[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < maxLag && dbuf[(size_t) tau + 1] < dbuf[(size_t) tau]) ++tau;
                best = tau; break;
            }
        if (best < 0)
        {
            float mn = 1.0f;
            for (int tau = minLag; tau < maxLag; ++tau) if (dbuf[(size_t) tau] < mn) { mn = dbuf[(size_t) tau]; best = tau; }
            if (mn > 0.35f) { confidence *= 0.6f; if (confidence < 0.2f) periodSamples = 0.0f; return; }
        }
        const float a = dbuf[(size_t) best - 1], b = dbuf[(size_t) best], c = dbuf[(size_t) best + 1];
        const float den = a - 2.0f * b + c;
        const float off = std::abs (den) > 1.0e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den) : 0.0f;
        periodSamples = ((float) best + off) * (float) kDec;
        confidence = 1.0f - juce::jlimit (0.0f, 1.0f, b);
    }
};

// ---------------------------------------------------------------
// TD-PSOLA voice: pitch-synchronous grains taken around the input's pitch
// marks and overlap-added at a new spacing (pitch) after resampling (formant).
// Latency ~2 input periods. Unvoiced input falls back to fixed 8 ms grains.
struct Psola
{
    static constexpr int kSize = 1 << 15;
    std::vector<float> in, acc, wsum;
    long long wpos = 0;          // input samples written
    long long outPos = 0;        // output samples emitted
    double nextMark = 0.0;       // next synthesis mark (output time)
    double lastCentre = -1.0;    // last analysis centre (input time)
    double sr = 44100.0;
    void prepare (double s) { sr = s; in.assign (kSize, 0.0f); acc.assign (kSize, 0.0f); wsum.assign (kSize, 0.0f); reset(); }
    void reset()
    {
        std::fill (in.begin(), in.end(), 0.0f); std::fill (acc.begin(), acc.end(), 0.0f); std::fill (wsum.begin(), wsum.end(), 0.0f);
        wpos = 0; outPos = 0; nextMark = 0.0; lastCentre = -1.0;
    }
    inline float inAt (double t) const
    {
        const long long i0 = (long long) std::floor (t);
        const float fr = (float) (t - (double) i0);
        if (i0 < wpos - kSize + 2 || i0 + 1 >= wpos) return 0.0f;
        return in[(size_t) (i0 & (kSize - 1))] * (1.0f - fr) + in[(size_t) ((i0 + 1) & (kSize - 1))] * fr;
    }
    // period: input period in samples (0 = unvoiced); pitchRatio: output/input pitch;
    // formantRatio: spectral envelope scale; fixedOutPeriod > 0 forces a monotone (robot)
    inline float process (float x, float period, float pitchRatio, float formantRatio, float fixedOutPeriod = 0.0f)
    {
        in[(size_t) (wpos & (kSize - 1))] = x;
        ++wpos;
        const bool voiced = period > 16.0f;
        const float P = voiced ? period : (float) (0.008 * sr);
        const float outP = fixedOutPeriod > 0.0f ? fixedOutPeriod : P / juce::jlimit (0.25f, 4.0f, pitchRatio);
        const float r = juce::jlimit (0.25f, 4.0f, formantRatio);
        const float half = P / r;                       // half grain length in output samples
        // place every grain whose window starts now
        int guard = 0;
        while ((double) outPos >= nextMark - half && guard++ < 8)
        {
            // analysis centre: pitch-synchronous step from the last one, tracking the input
            const double target = (double) wpos - P - 2.0;
            double c = target;
            if (voiced && lastCentre > 0.0)
            {
                const double steps = std::round ((target - lastCentre) / P);
                c = lastCentre + steps * P;
                if (c > target) c -= P;
            }
            lastCentre = c;
            const int len = (int) (2.0f * half);
            const long long start = (long long) std::floor (nextMark - half);
            for (int j = 0; j < len; ++j)
            {
                const float t = ((float) j + 0.5f) / (float) len;
                const float wv = 0.5f - 0.5f * std::cos (kTwoPi * t);
                const double src = c + ((double) j - half) * r;
                const long long o = start + j;
                if (o < outPos) continue;
                acc[(size_t) (o & (kSize - 1))] += wv * inAt (src);
                wsum[(size_t) (o & (kSize - 1))] += wv;
            }
            nextMark += outP;
        }
        // emit (output lags the input by ~2 periods)
        const size_t k = (size_t) (outPos & (kSize - 1));
        const float ws = wsum[k];
        const float y = ws > 0.05f ? acc[k] / juce::jmax (ws, 0.6f) : 0.0f;
        acc[k] = 0.0f; wsum[k] = 0.0f;
        ++outPos;
        if (nextMark < (double) outPos) nextMark = (double) outPos;
        return y;
    }
};

// ---------------------------------------------------------------
// FDN reverb: 16 lines, Hadamard mixing, Jot absorption (DC from RT60, HF
// from DAMP), cubic-interpolated per-line modulation, input diffusion.
struct FdnReverb
{
    static constexpr int N = 16, NAP = 4;
    static constexpr float kMaxScale = 2.2f;
    float sr = 44100.0f;
    std::vector<float> line[N];
    AllpassDelay ap[NAP];
    int pos[N] = {};
    float damp[N] = {}, lfo[N] = {}, lfoInc[N] = {};
    float gLo[N] = {}, aHi[N] = {};                 // cached per-line absorption
    float cRt = -1.0f, cSize = -1.0f, cDamp = -1.0f;
    static const float* lineMs() { static const float m[N] = { 21.7f,24.3f,27.1f,29.9f,33.1f,36.7f,40.3f,44.3f,48.7f,53.3f,58.1f,63.7f,69.1f,75.7f,82.3f,89.9f }; return m; }
    static const float* apMs() { static const float m[NAP] = { 4.7f, 3.6f, 12.7f, 9.3f }; return m; }

    void prepare (float s)
    {
        sr = s;
        for (int i = 0; i < N; ++i) line[i].assign ((size_t) (lineMs()[i] * 0.001f * kMaxScale * sr) + 256, 0.0f);
        for (auto& a : ap) a.prepare (s, 0.05f);
        reset();
    }
    void reset()
    {
        for (auto& b : line) std::fill (b.begin(), b.end(), 0.0f);
        for (auto& a : ap) a.reset();
        cRt = -1.0f;
        for (int i = 0; i < N; ++i)
        {
            pos[i] = 0; damp[i] = 0.0f; lfo[i] = (float) i * 2.399f;
            lfoInc[i] = kTwoPi * (0.13f + 0.071f * (float) ((i * 7) % N)) / sr;
        }
    }
    // size 0..1, dampHz = frequency where decay is noticeably faster, diffusion 0..1, mod 0..1
    void process (float inL, float inR, float rt60, float size, float dampHz, float diffusion, float mod,
                  float& outL, float& outR)
    {
        const float scale = 0.25f + (kMaxScale - 0.25f) * clamp01 (size);
        float x = 0.5f * (inL + inR);
        const float g = 0.75f * clamp01 (diffusion);
        for (int k = 0; k < NAP; ++k) x = ap[k].process (x, apMs()[k] * 0.001f * sr * (0.6f + 0.4f * scale), g);
        const float side = 0.5f * (inL - inR);
        if (rt60 != cRt || size != cSize || dampHz != cDamp)
        {
            cRt = rt60; cSize = size; cDamp = dampHz;
            const float t60 = juce::jmax (0.05f, rt60) * sr;
            const float t60hf = t60 * juce::jlimit (0.08f, 1.0f, std::pow (dampHz / 16000.0f, 0.7f));
            for (int i = 0; i < N; ++i)
            {
                const float dl = lineMs()[i] * 0.001f * scale * sr;
                const float gl = std::pow (10.0f, -3.0f * dl / t60), gh = std::pow (10.0f, -3.0f * dl / t60hf);
                aHi[i] = (gl - gh) / (gl + gh);
                gLo[i] = gl * (1.0f - aHi[i]);
            }
        }
        const float modDepth = 0.004f * sr * clamp01 (mod);
        float sgl[N];
        for (int i = 0; i < N; ++i)
        {
            auto& b = line[i]; const int n = (int) b.size();
            float dly = lineMs()[i] * 0.001f * scale * sr;
            lfo[i] += lfoInc[i]; if (lfo[i] > kTwoPi) lfo[i] -= kTwoPi;
            dly = juce::jlimit (4.0f, (float) (n - 4), dly + modDepth * std::sin (lfo[i]));
            float rp = (float) pos[i] - dly; if (rp < 0.0f) rp += (float) n;
            const int i0 = (int) rp; const float fr = rp - (float) i0;
            const float y0 = b[(size_t) ((i0 - 1 + n) % n)], y1 = b[(size_t) (i0 % n)], y2 = b[(size_t) ((i0 + 1) % n)], y3 = b[(size_t) ((i0 + 2) % n)];
            const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            const float rd = ((c3 * fr + c2) * fr + c1) * fr + y1;
            damp[i] = gLo[i] * rd + aHi[i] * damp[i];
            sgl[i] = damp[i];
        }
        for (int h = 1; h < N; h <<= 1)
            for (int i = 0; i < N; i += h << 1)
                for (int j = i; j < i + h; ++j) { const float a = sgl[j], c = sgl[j + h]; sgl[j] = a + c; sgl[j + h] = a - c; }
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float fb = sgl[i] * 0.25f;
            const float inj = (i & 1) ? (x - side) : (x + side);
            float wv = fb + inj * (((i * 5 + 3) % 7 < 3) ? -0.25f : 0.25f);
            if (! std::isfinite (wv)) wv = 0.0f;
            line[i][(size_t) pos[i]] = wv;
            if (++pos[i] >= (int) line[i].size()) pos[i] = 0;
            l += fb * (((i * 3) % 5 < 2) ? -1.0f : 1.0f);
            r += fb * (((i * 7 + 1) % 4 < 2) ? -1.0f : 1.0f);
        }
        outL = l * 0.35f; outR = r * 0.35f;
    }
};

// ---------------------------------------------------------------
// Dattorro plate (1997), delay lengths scaled from 29761 Hz.
struct DattorroPlate
{
    float sr = 44100.0f, k = 1.0f;
    OnePole bw;
    AllpassDelay inAp[4];
    AllpassDelay dec1[2], dec2[2];
    DelayLine d1[2], d2[2];
    OnePole dampF[2];
    float fbL = 0.0f, fbR = 0.0f, lfoPh = 0.0f;
    void prepare (float s)
    {
        sr = s; k = s / 29761.0f;
        for (auto& a : inAp) a.prepare (s, 0.03f);
        for (auto& a : dec1) a.prepare (s, 0.05f);
        for (auto& a : dec2) a.prepare (s, 0.12f);
        for (auto& d : d1) d.prepare (s, 0.2f);
        for (auto& d : d2) d.prepare (s, 0.2f);
        reset();
    }
    void reset()
    {
        bw.reset();
        for (auto& a : inAp) a.reset();
        for (auto& a : dec1) a.reset();
        for (auto& a : dec2) a.reset();
        for (auto& d : d1) d.reset();
        for (auto& d : d2) d.reset();
        for (auto& f : dampF) f.reset();
        fbL = fbR = 0.0f; lfoPh = 0.0f;
    }
    void process (float in, float rt60, float dampHz, float diffusion, float mod, float& outL, float& outR)
    {
        bw.setCutoff (14000.0f, sr);
        for (auto& f : dampF) f.setCutoff (dampHz, sr);
        const float id1 = 0.75f * diffusion, id2 = 0.625f * diffusion;
        float x = bw.lp (in);
        x = inAp[0].process (x, 142 * k, id1); x = inAp[1].process (x, 107 * k, id1);
        x = inAp[2].process (x, 379 * k, id2); x = inAp[3].process (x, 277 * k, id2);
        const float halfLoop = (672 + 4453 + 1800 + 3720) / 29761.0f;
        const float decay = juce::jlimit (0.0f, 0.9995f, std::pow (10.0f, -3.0f * halfLoop / juce::jmax (0.1f, rt60)));
        const float dd2 = juce::jlimit (0.25f, 0.5f, decay + 0.15f);
        lfoPh += kTwoPi * 0.9f / sr; if (lfoPh > kTwoPi) lfoPh -= kTwoPi;
        const float exc = 16.0f * k * mod;
        // left half
        float a = x + fbR;
        a = dec1[0].process (a, 672 * k + exc * std::sin (lfoPh), -0.7f);
        d1[0].write (a);
        float b = dampF[0].lp (d1[0].read (4453 * k)) * decay;
        b = dec2[0].process (b, 1800 * k, dd2);
        d2[0].write (b);
        const float nextL = d2[0].read (3720 * k) * decay;
        // right half
        float c = x + fbL;
        c = dec1[1].process (c, 908 * k + exc * std::cos (lfoPh), -0.7f);
        d1[1].write (c);
        float e = dampF[1].lp (d1[1].read (4217 * k)) * decay;
        e = dec2[1].process (e, 2656 * k, dd2);
        d2[1].write (e);
        const float nextR = d2[1].read (3163 * k) * decay;
        fbL = sanitize (nextL); fbR = sanitize (nextR);
        auto t = [this] (const DelayLine& d, float smp) { return d.read (smp * k); };
        auto ta = [this] (const AllpassDelay& ap, float smp) { return ap.d.read (smp * k); };
        outL = 0.6f * (t (d1[1], 266) + t (d1[1], 2974) - ta (dec2[1], 1913) + t (d2[1], 1996)
                       - t (d1[0], 1990) - ta (dec2[0], 187) - t (d2[0], 1066));
        outR = 0.6f * (t (d1[0], 353) + t (d1[0], 3627) - ta (dec2[0], 1228) + t (d2[0], 2673)
                       - t (d1[1], 2111) - ta (dec2[1], 335) - t (d2[1], 121));
    }
};

// ---------------------------------------------------------------
// stereo 2x / 4x oversampler (IIR polyphase half-band, ~1-2 samples latency)
struct Oversampler
{
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    std::vector<float> scratchL, scratchR;
    int factorLog2 = 1;
    void prepare (int maxBlock, int log2Factor = 1)
    {
        factorLog2 = log2Factor;
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) log2Factor,
                                                                 juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os->initProcessing ((size_t) maxBlock);
    }
    void reset() { if (os) os->reset(); }
    int factor() const { return 1 << factorLog2; }
    // fn (float& l, float& r) runs once per oversampled sample
    template <typename Fn>
    void process (float* L, float* R, int n, Fn&& fn)
    {
        float* chans[2] = { L, R };
        juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
        auto up = os->processSamplesUp (block);
        float* uL = up.getChannelPointer (0);
        float* uR = up.getChannelPointer (1);
        const int un = (int) up.getNumSamples();
        for (int i = 0; i < un; ++i) fn (uL[i], uR[i]);
        os->processSamplesDown (block);
    }
};

} // namespace efx
