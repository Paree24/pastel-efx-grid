#include "FxCommon.h"

// Filters. Meter conventions (VizTaps::m):
//   m[0] live (modulated) cutoff / centre Hz, or scan position for the
//        wavetable filter / vowel position for the formant filter
// responseDb(): magnitude at the *static* settings for the curve display.

namespace efx
{
namespace
{

inline float driveIn (float x, float drive01)
{
    if (drive01 <= 0.001f) return x;
    const float g = 1.0f + drive01 * 8.0f;
    return fastTanh (x * g) / std::sqrt (g);
}

// drive in dB: up to +36 dB into a tanh, only partly compensated so it is heard
inline float driveDb (float x, float gain)
{
    if (gain <= 1.001f) return x;
    return fastTanh (x * gain) / std::pow (gain, 0.6f);
}

// ------------------------------------------------------------------ Multimode
// Cutoff, Reso, Drive, Env Amt, Env Rel, Slope, Out, Env Atk | LP / HP / BP / Notch
class Multimode : public FxImpl
{
public:
    void prepare (double s) override { for (auto* f : { &a[0], &a[1], &b[0], &b[1] }) f->prepare (s); reset(); }
    void reset() override { for (auto* f : { &a[0], &a[1], &b[0], &b[1] }) f->reset(); env.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float cut = c.p[0], res = c.p[1], drive = dbToGain (c.p[2]), envAmt = c.p[3];
        env.set (c.p[7], c.p[4], sr);
        const bool steep = ci (c, 5) == 1;
        const float out = dbToGain (c.p[6]);
        const int mode = c.mode;
        float fc = cut;
        for (int i = 0; i < n; ++i)
        {
            const float e = clamp01 (env.process (0.5f * (L[i] + R[i])) * 3.0f);
            fc = juce::jlimit (20.0f, 20000.0f, cut * std::pow (2.0f, envAmt * 6.0f * e));
            float l = driveDb (L[i], drive), r = driveDb (R[i], drive);
            l = a[0].processNLMode (l, fc, res, mode); r = a[1].processNLMode (r, fc, res, mode);
            if (steep) { l = b[0].processNLMode (l, fc, res * 0.4f, mode); r = b[1].processNLMode (r, fc, res * 0.4f, mode); }
            L[i] = l * out; R[i] = r * out;
        }
        taps->set (0, fc);
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double, float& db) const override
    {
        db = Svf::responseDb (mode, hz, p[0], p[1]) + p[6];
        if (mode == 2) db += 10.0f * std::log10 (Svf::resToQ (p[1]));   // band-pass peaks at sqrt(Q)
        if (ci (p, 5) == 1) db += Svf::responseDb (mode, hz, p[0], p[1] * 0.4f);
        return true;
    }
private:
    Svf a[2], b[2];
    EnvFollower env;
};

// ------------------------------------------------------------------ SVF morph
// Cutoff, Reso, Morph, Spread, Drive, Out | 12 dB / 24 dB
class SvfMorph : public FxImpl
{
public:
    void prepare (double s) override { for (auto& f : f1) f.prepare (s); for (auto& f : f2) f.prepare (s); reset(); }
    void reset() override { for (auto& f : f1) f.reset(); for (auto& f : f2) f.reset(); }
    static float morph (float lp, float bpn, float hp, float m)
    {
        return m < 0.5f ? lerp (lp, bpn, m * 2.0f) : lerp (bpn, hp, m * 2.0f - 1.0f);
    }
    inline float stage (Svf& f, float x, float fc, float res, float m)
    {
        float lp, bp, hp;
        const float q = Svf::resToQ (res);
        f.processNL (x, fc, q, lp, bp, hp);
        return morph (lp, bp / std::sqrt (q) * 1.4f, hp, m);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float fcL = c.p[0] * std::pow (2.0f, -c.p[3] * 0.5f), fcR = c.p[0] * std::pow (2.0f, c.p[3] * 0.5f);
        const float res = c.p[1], m = c.p[2], drive = dbToGain (c.p[4]), out = dbToGain (c.p[5]);
        const bool steep = c.mode == 1;
        for (int i = 0; i < n; ++i)
        {
            float l = stage (f1[0], driveDb (L[i], drive), fcL, res, m);
            float r = stage (f1[1], driveDb (R[i], drive), fcR, res, m);
            if (steep) { l = stage (f2[0], l, fcL, res * 0.4f, m); r = stage (f2[1], r, fcR, res * 0.4f, m); }
            L[i] = l * out; R[i] = r * out;
        }
        taps->set (0, c.p[0]);
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double, float& db) const override
    {
        auto one = [&] (float res)
        {
            const float lp = dbToGain (Svf::responseDb (0, hz, p[0], res));
            const float bp = dbToGain (Svf::responseDb (2, hz, p[0], res)) * std::sqrt (Svf::resToQ (res)) * 1.4f;   // NL stage: peak sqrt(Q)
            const float hp = dbToGain (Svf::responseDb (1, hz, p[0], res));
            return gainToDb (std::abs (morph (lp, bp, hp, p[2])));
        };
        db = one (p[1]) + (mode == 1 ? one (p[1] * 0.4f) : 0.0f) + p[5];
        return true;
    }
private:
    Svf f1[2], f2[2];
};

// ------------------------------------------------------------------ Comb
// Freq, Feedback, Damp, Spread, Out | Feedback / Feedfwd
class CombFx : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 0.06f); dr.prepare (s, 0.06f); reset(); }
    void reset() override { dl.reset(); dr.reset(); lpL.reset(); lpR.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float fb = c.p[1];
        const float dL = (float) sr / c.p[0];
        const float dR = (float) sr / (c.p[0] * std::pow (2.0f, c.p[3] / 1200.0f));
        lpL.setCutoff (c.p[2], sr); lpR.setCutoff (c.p[2], sr);
        const float norm = std::pow (1.0f - std::abs (fb), 0.7f);
        const float out = dbToGain (c.p[4]);
        const bool ff = c.mode == 1;
        for (int i = 0; i < n; ++i)
        {
            const float yl = dl.read (dL), yr = dr.read (dR);
            if (ff)
            {
                dl.write (L[i]); dr.write (R[i]);
                L[i] = (L[i] + fb * lpL.lp (yl)) * out;
                R[i] = (R[i] + fb * lpR.lp (yr)) * out;
            }
            else
            {
                const float wl = L[i] + fb * lpL.lp (yl), wr = R[i] + fb * lpR.lp (yr);
                dl.write (loopSat (wl)); dr.write (loopSat (wr));
                L[i] = wl * norm * out; R[i] = wr * norm * out;
            }
        }
        taps->set (0, c.p[0]);
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double s, float& db) const override
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * hz / s, D = s / p[0];
        const std::complex<double> z = std::polar (1.0, -w * D);
        const double g = p[1];
        const double m = mode == 1 ? std::abs (1.0 + g * z) : std::abs (1.0 / (1.0 - g * z)) * std::pow (1.0 - std::abs (g), 0.7);
        db = (float) (20.0 * std::log10 (juce::jmax (1.0e-6, m))) + p[4];
        return true;
    }
private:
    DelayLine dl, dr;
    OnePole lpL, lpR;
};

// ------------------------------------------------------------------ Formant
// Vowel, Shift, Reso, LFO Rate, LFO Depth, Drive, Out | Male / Female
class FormantFx : public FxImpl
{
public:
    static constexpr float kMale[5][3]   = { { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } };
    static constexpr float kFemale[5][3] = { { 850, 1220, 2810 }, { 610, 2330, 2990 }, { 310, 2790, 3310 }, { 590, 920, 2710 }, { 370, 950, 2670 } };
    void prepare (double s) override { for (auto& f : fl) f.prepare (s); for (auto& f : fr) f.prepare (s); reset(); }
    void reset() override { for (auto& f : fl) f.reset(); for (auto& f : fr) f.reset(); lfo.reset(); }
    static void formants (float v, int mode, float shift, float* f)
    {
        const auto& t = mode == 0 ? kMale : kFemale;
        v = juce::jlimit (0.0f, 4.0f, v);
        const int i0 = juce::jmin (3, (int) v);
        const float fr = v - (float) i0;
        const float sc = std::pow (2.0f, shift / 12.0f);
        for (int k = 0; k < 3; ++k) f[k] = (t[i0][k] + (t[i0 + 1][k] - t[i0][k]) * fr) * sc;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float q = 4.0f + c.p[2] * 20.0f;
        const float drive = c.p[5], out = dbToGain (c.p[6]) * (0.6f + 0.1f * q);
        static constexpr float gains[3] = { 1.0f, 0.6f, 0.35f };
        float v = c.p[0];
        for (int i = 0; i < n; ++i)
        {
            v = c.p[0] + c.p[4] * 2.0f * lfo.valueAt (0, 0.0);
            if (v < 0.0f) v = -v;
            if (v > 4.0f) v = 8.0f - v;
            lfo.advance (c.p[3] / sr);
            float f[3];
            formants (v, c.mode, c.p[1], f);
            const float il = driveIn (L[i], drive), ir = driveIn (R[i], drive);
            float ol = 0.0f, orr = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                float lp, bp, hp;
                fl[k].processQ (il, f[k], q, lp, bp, hp); ol += bp / q * gains[k];
                fr[k].processQ (ir, f[k], q, lp, bp, hp); orr += bp / q * gains[k];
            }
            L[i] = ol * out; R[i] = orr * out;
        }
        taps->set (0, v);
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double, float& db) const override
    {
        float f[3]; formants (p[0], mode, p[1], f);
        const float q = 4.0f + p[2] * 20.0f;
        static constexpr float gains[3] = { 1.0f, 0.6f, 0.35f };
        std::complex<double> sum = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            const std::complex<double> s (0.0, hz / f[k]);
            sum += (double) gains[k] * (s / (double) q) / (s * s + s / (double) q + 1.0);
        }
        db = (float) (20.0 * std::log10 (juce::jmax (1.0e-6, std::abs (sum)))) + gainToDb (0.6f + 0.1f * q) + p[6];
        return true;
    }
private:
    Svf fl[3], fr[3];
    Lfo lfo;
};

// ------------------------------------------------------------------ Ladder
// Cutoff, Reso, Drive, Env Amt, Bass, Out | 24 dB / 12 dB
class LadderFx : public FxImpl
{
public:
    void prepare (double s) override { l.prepare (s); r.prepare (s); reset(); }
    void reset() override { l.reset(); r.reset(); env.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        env.set (2.0f, 150.0f, sr);
        const float drive = dbToGain (c.p[2]), out = dbToGain (c.p[5]) * std::pow (drive, 0.25f);
        const bool two = c.mode == 1;
        float fc = c.p[0];
        for (int i = 0; i < n; ++i)
        {
            const float e = clamp01 (env.process (0.5f * (L[i] + R[i])) * 3.0f);
            fc = juce::jlimit (20.0f, 20000.0f, c.p[0] * std::pow (2.0f, c.p[3] * 6.0f * e));
            L[i] = l.process (L[i], fc, c.p[1], drive, two, c.p[4]) * out;
            R[i] = r.process (R[i], fc, c.p[1], drive, two, c.p[4]) * out;
        }
        taps->set (0, fc);
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double, float& db) const override
    {
        const std::complex<double> s (0.0, hz / p[0]);
        const double k = p[1] * 4.1;
        const auto stage = 1.0 / (1.0 + s);
        const auto h = std::pow (stage, 4) / (1.0 + k * std::pow (stage, 4));
        const auto h2 = std::pow (stage, 2) / (1.0 + k * std::pow (stage, 4));
        const double bass = 1.0 + p[4] * k * 0.5;
        db = (float) (20.0 * std::log10 (juce::jmax (1.0e-6, std::abs (mode == 1 ? h2 : h) * bass))) + p[5];
        return true;
    }
private:
    Ladder l, r;
    EnvFollower env;
};

// ------------------------------------------------------------------ Vintage
// Cutoff, Reso, Drive, Age, Tone, Out | Gtr / Bass
class Vintage : public FxImpl
{
public:
    void prepare (double s) override { fl.prepare (s); fr.prepare (s); rng.seed (77); reset(); }
    void reset() override { fl.reset(); fr.reset(); drift = 0.0f; driftT = 0.0f; ageL.reset(); ageR.reset(); tilt[0].reset(); tilt[1].reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float age = c.p[3];
        // slow random drift of the cutoff (re-targeted every chunk)
        if (rng.uni() < 0.02f) driftT = rng.bipolar();
        drift += (driftT - drift) * 0.01f;
        const float fc = c.p[0] * std::pow (2.0f, drift * age * 0.5f);
        ageL.setCutoff (18000.0f * std::pow (0.33f, age), sr); ageR.setCutoff (18000.0f * std::pow (0.33f, age), sr);
        tilt[0].set (c.p[4], sr); tilt[1].set (c.p[4], sr);
        const float drive = 1.0f + c.p[2] * 6.0f, out = dbToGain (c.p[5]);
        const bool gtr = c.mode == 0;
        const float q = Svf::resToQ (c.p[1] * 0.85f);
        const float hiss = age * 0.0008f;
        for (int i = 0; i < n; ++i)
        {
            auto chan = [&] (Svf& f, OnePole& ag, Tilt& t, float x)
            {
                x = fastTanh ((x + 0.05f * c.p[2]) * drive) / std::sqrt (drive) - fastTanh (0.05f * c.p[2] * drive) / std::sqrt (drive);
                float lp, bp, hp;
                f.processQ (x, fc, q, lp, bp, hp);
                const float y = gtr ? bp / q * 1.5f + lp * 0.25f : lp + 0.5f * bp / q;
                return t.process (ag.lp (y + rng.bipolar() * hiss));
            };
            L[i] = chan (fl, ageL, tilt[0], L[i]) * out;
            R[i] = chan (fr, ageR, tilt[1], R[i]) * out;
        }
        taps->set (0, fc);
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double, float& db) const override
    {
        const float q = Svf::resToQ (p[1] * 0.85f);
        const std::complex<double> s (0.0, hz / p[0]);
        const auto den = s * s + s / (double) q + 1.0;
        const auto h = mode == 0 ? (s / (double) q * 1.5 + 0.25) / den : (1.0 + 0.5 * s / (double) q) / den;
        db = (float) (20.0 * std::log10 (juce::jmax (1.0e-6, std::abs (h)))) + p[5];
        return true;
    }
private:
    Svf fl, fr;
    OnePole ageL, ageR;
    Tilt tilt[2];
    Rng rng;
    float drift = 0.0f, driftT = 0.0f;
};

// ------------------------------------------------------------------ Wavetable filter
// Pos, Freq, Reso, Rate, Depth, Bright, Out, Type, Contrast | LFO / Env
// Harmonic: a resonator bank at the harmonics of Freq, each harmonic's level
//   taken from the current frame's spectrum (the input takes on its timbre).
// Spectral: 32 log-spaced bands from Freq/2 to 16 kHz whose levels follow the
//   frame's spectrum stretched over the whole range - a strong, pitch-free
//   colouring that changes clearly as the frame scans.
class WavetableFilter : public FxImpl
{
public:
    static constexpr int H = 32;
    void prepare (double) override { reset(); }
    void reset() override { for (auto& b : bl) b.reset(); for (auto& b : br) b.reset(); lfo.reset(); env.reset(); pos = 0.0f; }
    static float levelFor (const Wavetable* t, float pos, int band, bool spectral, float bright, float contrast)
    {
        float a;
        if (spectral)
        {
            const float hf = (float) band / (float) (H - 1) * (float) (Wavetable::kHarm - 1);
            const int h0 = (int) hf;
            a = lerp (t->harmonic (pos, h0), t->harmonic (pos, juce::jmin (Wavetable::kHarm - 1, h0 + 1)), hf - (float) h0);
        }
        else a = t->harmonic (pos, band);
        a *= std::pow ((float) (band + 1), bright * 0.8f);
        return std::pow (clamp01 (a), 0.5f + contrast * 2.5f);
    }
    static float bandFreq (int band, float f0, bool spectral, double sr)
    {
        if (! spectral) return f0 * (float) (band + 1);
        const float lo = f0 * 0.5f, hi = juce::jmin (16000.0f, (float) sr * 0.45f);
        return lo * std::pow (hi / lo, (float) band / (float) (H - 1));
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const auto* t = c.table != nullptr ? c.table : Wavetable::builtIn().get();
        float mod;
        if (c.mode == 0) { mod = 0.5f * lfo.valueAt (0, 0.0); lfo.advance (c.p[3] * n / sr); }
        else
        {
            float pk = 0.0f;
            for (int i = 0; i < n; ++i) pk = juce::jmax (pk, std::abs (L[i]), std::abs (R[i]));
            env.set (5.0f, 200.0f, sr / n);
            mod = clamp01 (env.process (pk) * 3.0f);
        }
        pos = clamp01 (c.p[0] + c.p[4] * mod);
        const bool spectral = ci (c, 7) == 1;
        const float f0 = c.p[1], reso = c.p[2], contrast = c.p[8];
        float sumG = 0.0f;
        used = 0;
        for (int h = 0; h < H; ++h)
        {
            const float f = bandFreq (h, f0, spectral, sr);
            if (f > sr * 0.45) break;
            // harmonic: constant bandwidth; spectral: constant Q (bands overlap at reso 0)
            const float q = spectral ? 1.5f + reso * 18.0f : f / juce::jmax (1.0f, f0 * (0.5f - 0.47f * reso));
            bl[h].setBandPass (f, q, sr); br[h].copyCoeffs (bl[h]);
            g[h] = levelFor (t, pos, h, spectral, c.p[5], contrast);
            sumG += g[h];
            used = h + 1;
        }
        // keep loudness roughly constant across reso / table / contrast
        const float avgQ = spectral ? 1.5f + reso * 18.0f : 1.0f / (0.5f - 0.47f * reso);
        const float makeup = std::sqrt (avgQ) * (spectral ? 2.2f : 0.55f) / juce::jmax (0.15f, std::sqrt (sumG));
        const float out = dbToGain (c.p[6]) * juce::jmin (16.0f, makeup);
        for (int i = 0; i < n; ++i)
        {
            float ol = 0.0f, orr = 0.0f;
            for (int h = 0; h < used; ++h) { ol += bl[h].process (L[i]) * g[h]; orr += br[h].process (R[i]) * g[h]; }
            L[i] = ol * out; R[i] = orr * out;
        }
        taps->set (0, pos);
    }
    bool responseDb (float hz, const float* p, int, const float*, double s, float& db) const override
    {
        const auto* t = currentTable();
        const bool spectral = ci (p, 7) == 1;
        const float position = taps != nullptr ? taps->get (0) : p[0];
        std::complex<double> sum = 0.0;
        float sumG = 0.0f;
        for (int h = 0; h < H; ++h)
        {
            const float f = bandFreq (h, p[1], spectral, s);
            if (f > s * 0.45) break;
            const float q = spectral ? 1.5f + p[2] * 18.0f : f / juce::jmax (1.0f, p[1] * (0.5f - 0.47f * p[2]));
            const float gh = levelFor (t, position, h, spectral, p[5], p[8]);
            const std::complex<double> sx (0.0, hz / f);
            sum += (double) gh * (sx / (double) q) / (sx * sx + sx / (double) q + 1.0);
            sumG += gh;
        }
        const float avgQ = spectral ? 1.5f + p[2] * 18.0f : 1.0f / (0.5f - 0.47f * p[2]);
        const float makeup = juce::jmin (16.0f, std::sqrt (avgQ) * (spectral ? 2.2f : 0.55f) / juce::jmax (0.15f, std::sqrt (sumG)));
        db = (float) (20.0 * std::log10 (juce::jmax (1.0e-6, std::abs (sum) * makeup))) + p[6];
        return true;
    }
private:
    Biquad bl[H], br[H];
    float g[H] {};
    float pos = 0.0f;
    int used = 0;
    Lfo lfo;
    EnvFollower env;
};

// ------------------------------------------------------------------ All-pass disperser
// Freq, Q, Stages, Spread, Feedback, LFO Rate, LFO Depth
class AllpassFx : public FxImpl
{
public:
    static constexpr int kMax = 64;
    void prepare (double) override { reset(); }
    void reset() override { for (auto& b : al) b.reset(); for (auto& b : ar) b.reset(); fbL = fbR = 0.0f; key = -1.0f; lfo.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int stages = juce::jlimit (1, kMax, ci (c, 2));
        const float mod = c.p[6] > 0.001f ? c.p[6] * lfo.valueAt (0, 0.0) : 0.0f;
        lfo.advance (c.p[5] * n / sr);
        const float centre = c.p[0] * std::pow (2.0f, mod);
        const float k = centre + c.p[1] * 1000.0f + (float) stages * 1.0e5f + c.p[3] * 1.0e7f;
        if (std::abs (k - key) > 1.0e-3f)
        {
            key = k;
            for (int s = 0; s < stages; ++s)
            {
                const float t = stages > 1 ? (float) s / (float) (stages - 1) - 0.5f : 0.0f;
                al[s].setAllPass (centre * std::pow (2.0f, t * c.p[3]), c.p[1], sr);
                ar[s].copyCoeffs (al[s]);
            }
        }
        const float fb = c.p[4];
        for (int i = 0; i < n; ++i)
        {
            float l = L[i] + fbL * fb, r = R[i] + fbR * fb;
            for (int s = 0; s < stages; ++s) { l = al[s].process (l); r = ar[s].process (r); }
            fbL = loopSat (l); fbR = loopSat (r);
            L[i] = l * (1.0f - std::abs (fb) * 0.5f); R[i] = r * (1.0f - std::abs (fb) * 0.5f);
        }
        taps->set (0, centre);
    }
private:
    Biquad al[kMax], ar[kMax];
    float fbL = 0.0f, fbR = 0.0f, key = -1.0f;
    Lfo lfo;
};

} // namespace

std::unique_ptr<FxImpl> makeFilterFx (const juce::String& n)
{
    if (n == "Multimode Filter") return std::make_unique<Multimode>();
    if (n == "SVF Filter")       return std::make_unique<SvfMorph>();
    if (n == "Comb Filter")      return std::make_unique<CombFx>();
    if (n == "Formant Filter")   return std::make_unique<FormantFx>();
    if (n == "Ladder Filter")    return std::make_unique<LadderFx>();
    if (n == "Vintage Filter")   return std::make_unique<Vintage>();
    if (n == "Wavetable Filter") return std::make_unique<WavetableFilter>();
    if (n == "All-Pass Filter")  return std::make_unique<AllpassFx>();
    return nullptr;
}

} // namespace efx
