#include "FxCommon.h"

// Drive. Nonlinear stages run 2x oversampled (Clipper: 2x or 4x).
// transfer(): x in -1..1 (input sample) -> y (output sample), excluding
// filters, for the transfer-curve display. Meter conventions:
//   m[0] input peak (lin), m[1] scan position (wavetable drive) / clip dB

namespace efx
{
namespace
{

// shared shell: pre/post filters + oversampled shaper + DC blocker
class DriveBase : public FxImpl
{
public:
    void prepare (double s) override { os.prepare (GridEffect::kChunk, osLog2); osSr = s * os.factor(); reset(); }
    void reset() override { os.reset(); dcL.reset(); dcR.reset(); resetState(); }
    virtual void resetState() {}
protected:
    Oversampler os;
    int osLog2 = 1;
    double osSr = 88200.0;
    DcBlocker dcL, dcR;
    void dc (float* L, float* R, int n) { for (int i = 0; i < n; ++i) { L[i] = dcL.process (L[i]); R[i] = dcR.process (R[i]); } }
    void peak (const float* L, const float* R, int n)
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i) p = juce::jmax (p, std::abs (L[i]), std::abs (R[i]));
        taps->set (0, juce::jmax (p, taps->get (0) * 0.9f));
    }
};

// ------------------------------------------------------------------ Soft Clip
// Drive, Hardness, Bias, Tone, Pre HP, Out, Ceiling | Soft / Hard
class SoftClip : public DriveBase
{
public:
    void resetState() override { hpL.reset(); hpR.reset(); tilt[0].reset(); tilt[1].reset(); lastHp = -1.0f; }
    static float shape (float x, float k)
    {
        return x / std::pow (1.0f + std::pow (std::abs (x), k), 1.0f / k);
    }
    static float curve (float x, const float* p, int mode)
    {
        const float g = dbToGain (p[0]), b = p[2] * 0.4f;
        if (mode == 1)
        {
            // true hard clip at the ceiling (level-matched so Drive doesn't just get louder)
            const float ceil = dbToGain (p[6]);
            auto hc = [ceil] (float v) { return juce::jlimit (-ceil, ceil, v); };
            return (hc (x * g + b) - hc (b)) / juce::jmax (ceil, 0.05f) * dbToGain (p[5]) * 0.8f;
        }
        const float k = 1.0f + p[1] * 9.0f;
        const float comp = 1.0f / juce::jmax (1.0f, std::sqrt (g) * 0.85f);
        return (shape (x * g + b, k) - shape (b, k)) * comp * dbToGain (p[5]);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        if (std::abs (c.p[4] - lastHp) > 0.01f) { hpL.setHighPass (c.p[4], 0.707f, sr); hpR.copyCoeffs (hpL); lastHp = c.p[4]; }
        if (c.p[4] > 21.0f) for (int i = 0; i < n; ++i) { L[i] = hpL.process (L[i]); R[i] = hpR.process (R[i]); }
        float p[kMaxParams]; std::copy (c.p, c.p + kMaxParams, p);
        const int mode = c.mode;
        os.process (L, R, n, [&] (float& l, float& r) { l = curve (l, p, mode); r = curve (r, p, mode); });
        dc (L, R, n);
        tilt[0].set (c.p[3], sr); tilt[1].set (c.p[3], sr);
        for (int i = 0; i < n; ++i) { L[i] = tilt[0].process (L[i]); R[i] = tilt[1].process (R[i]); }
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override { y = curve (x, p, mode); return true; }
private:
    Biquad hpL, hpR;
    Tilt tilt[2];
    float lastHp = -1.0f;
};

// ------------------------------------------------------------------ Saturation
// Drive, Tone, Bias, Out | Tube / Tape / Diode
class Saturation : public DriveBase
{
public:
    void resetState() override { for (auto& b : emph) b.reset(); for (auto& b : deemph) b.reset(); tilt[0].reset(); tilt[1].reset(); lastSr = 0.0; }
    static float sat (float x, int mode, float bias)
    {
        switch (mode)
        {
            case 0: { const float b = bias * 0.6f; return fastTanh (x + b) - fastTanh (b); }                // tube: even harmonics from bias
            case 1: { const float b = bias * 0.3f; const float y = x + b; return y / (1.0f + std::abs (y)) - b / (1.0f + std::abs (b)); }   // tape
            default:
            {
                const float y = x + bias * 0.3f;
                const float d = y > 0.0f ? 1.0f - std::exp (-y * 1.6f) : -0.55f * (1.0f - std::exp (y * 1.2f));
                const float d0 = bias > 0.0f ? 1.0f - std::exp (-bias * 0.48f) : 0.0f;
                return d - d0;
            }
        }
    }
    static float curve (float x, const float* p, int mode)
    {
        const float g = dbToGain (p[0]);
        return sat (x * g, mode, p[2]) / juce::jmax (0.6f, std::pow (g, 0.6f)) * dbToGain (p[3]) * 1.2f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        if (lastSr != osSr) { emph[0].setHighShelf (3000.0f, 0.7f, 6.0f, osSr); emph[1].copyCoeffs (emph[0]);
                              deemph[0].setHighShelf (3000.0f, 0.7f, -6.0f, osSr); deemph[1].copyCoeffs (deemph[0]); lastSr = osSr; }
        const int mode = c.mode;
        float p[kMaxParams]; std::copy (c.p, c.p + kMaxParams, p);
        os.process (L, R, n, [&] (float& l, float& r)
        {
            if (mode == 1)
            {
                l = deemph[0].process (curve (emph[0].process (l), p, mode));
                r = deemph[1].process (curve (emph[1].process (r), p, mode));
            }
            else { l = curve (l, p, mode); r = curve (r, p, mode); }
        });
        dc (L, R, n);
        tilt[0].set (c.p[1], sr); tilt[1].set (c.p[1], sr);
        for (int i = 0; i < n; ++i) { L[i] = tilt[0].process (L[i]); R[i] = tilt[1].process (R[i]); }
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override { y = curve (x, p, mode); return true; }
private:
    Biquad emph[2], deemph[2];
    Tilt tilt[2];
    double lastSr = 0.0;
};

// ------------------------------------------------------------------ Fuzz
// Fuzz, Tone, Gate, Starve, Octave, Out | Si / Ge
class Fuzz : public DriveBase
{
public:
    void resetState() override { env = 0.0f; gateG = 1.0f; lpL.reset(); lpR.reset(); hpL.reset(); hpR.reset(); }
    static float shaper (float x, int mode, float starve)
    {
        x += starve * 0.35f;
        if (std::abs (x) < starve * 0.08f) x = 0.0f;   // sputter
        return mode == 0 ? fastTanh (x * 2.0f) : (x > 0.0f ? fastTanh (x) : fastTanh (x * 0.6f) / 0.9f);
    }
    static float curve (float x, const float* p, int mode)
    {
        const float g = dbToGain (p[0] * 50.0f);
        const float oct = p[4];
        const float in = x * (1.0f - oct) + std::abs (x) * oct * 1.5f;
        return (shaper (in * g, mode, p[3]) - shaper (0.0f, mode, p[3])) * dbToGain (p[5]) * 0.45f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        const float gateTh = dbToGain (c.p[2]);
        const float envR = msCoef (40.0f, sr), gA = msCoef (2.0f, sr), gR = msCoef (60.0f, sr);
        for (int i = 0; i < n; ++i)
        {
            const float a = juce::jmax (std::abs (L[i]), std::abs (R[i]));
            env = a > env ? a : env + (a - env) * envR;
            const float t = env > gateTh ? 1.0f : 0.0f;
            gateG += (t > gateG ? gA : gR) * (t - gateG);
            L[i] *= gateG; R[i] *= gateG;
        }
        const int mode = c.mode;
        float p[kMaxParams]; std::copy (c.p, c.p + kMaxParams, p);
        os.process (L, R, n, [&] (float& l, float& r) { l = curve (l, p, mode); r = curve (r, p, mode); });
        dc (L, R, n);
        // muff-style tone: lowpass <-> highpass crossfade (mid scoop in the middle)
        lpL.setCutoff (700.0f, sr); lpR.setCutoff (700.0f, sr); hpL.setCutoff (1100.0f, sr); hpR.setCutoff (1100.0f, sr);
        const float t = c.p[1];
        for (int i = 0; i < n; ++i)
        {
            L[i] = lpL.lp (L[i]) * (1.0f - t) * 1.3f + hpL.hp (L[i]) * t * 1.3f;
            R[i] = lpR.lp (R[i]) * (1.0f - t) * 1.3f + hpR.hp (R[i]) * t * 1.3f;
        }
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override { y = curve (x, p, mode); return true; }
private:
    float env = 0.0f, gateG = 1.0f;
    OnePole lpL, lpR, hpL, hpR;
};

// ------------------------------------------------------------------ Phase distortion
// Amount, Mod Tone, Drive, Sym, Out
class PhaseDist : public DriveBase
{
public:
    void prepare (double s) override { DriveBase::prepare (s); dl.prepare (osSr, 0.01f); dr.prepare (osSr, 0.01f); resetState(); }
    void resetState() override { dl.reset(); dr.reset(); ml.reset(); mr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        const float A = c.p[0] * 0.002f * (float) osSr;
        const float drive = dbToGain (c.p[2]), sym = c.p[3], out = dbToGain (c.p[4]);
        ml.setCutoff (c.p[1], osSr); mr.setCutoff (c.p[1], osSr);
        auto mod = [sym] (float m) { return m >= 0.0f ? m * (1.0f - juce::jmax (0.0f, -sym)) : m * (1.0f - juce::jmax (0.0f, sym)); };
        os.process (L, R, n, [&] (float& l, float& r)
        {
            dl.write (l); dr.write (r);
            const float a = mod (fastTanh (ml.lp (l) * drive)), b = mod (fastTanh (mr.lp (r) * drive));
            l = dl.readCubic (A * (1.0f + a) + 3.0f) * out;
            r = dr.readCubic (A * (1.0f + b) + 3.0f) * out;
        });
    }
private:
    DelayLine dl, dr;
    OnePole ml, mr;
};

// ------------------------------------------------------------------ Waveshaper
// Curve, Drive, Sym, Pre Tone, Post Tone, Out, Morph
// Shape data: the Custom curve (CurvePoints over x 0..1, mirrored for x < 0).
class Waveshaper : public DriveBase
{
public:
    static constexpr int kCurves = 12, kCustom = 11;
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        CurvePoints::set (d, { { 0.0f, 0.0f, 0.0f }, { 0.35f, 0.6f, -0.3f }, { 0.7f, 0.85f, 0.0f }, { 1.0f, 0.95f, 0.0f } });
    }
    void resetState() override { preL.reset(); preR.reset(); postL.reset(); postR.reset(); }
    static float shape (int curve, float x, const float* custom)
    {
        switch (curve)
        {
            case 1: { const float t = x * 0.25f + 0.25f; const float w = t - std::floor (t); return -std::sin (kTwoPi * w - kPi * 0.5f); }  // sine fold
            case 2: { const float c = juce::jlimit (-1.0f, 1.0f, x); return 1.5f * c - 0.5f * c * c * c; }
            case 3: return juce::jlimit (-1.0f, 1.0f, x);
            case 4: return x > 0.0f ? fastTanh (x) : fastTanh (x * 0.4f) * 1.4f;
            case 5: { const float c = fastTanh (x); return 4.0f * c * c * c - 3.0f * c; }                      // Chebyshev T3
            case 6: { const float c = fastTanh (x), c2 = c * c; return c * (16.0f * c2 * c2 - 20.0f * c2 + 5.0f); }   // T5
            case 7: { float v = x; for (int i = 0; i < 6 && std::abs (v) > 1.0f; ++i) v = std::copysign (2.0f - std::abs (v), v); return v; }   // foldback
            case 8: { const float v = x + 0.2f; return (v > 0.0f ? 1.0f - std::exp (-v) : -(1.0f - std::exp (v * 1.6f)) / 1.6f) - 0.1813f; }  // tube
            case 9: return x > 0.0f ? fastTanh (x * 1.5f) : fastTanh (x * 0.25f) * 0.5f;                      // diode
            case 10: return fastTanh (std::abs (x)) * 2.0f - 0.8f + x * 0.1f;                                 // rectify (octave up)
            case 11:
                if (custom == nullptr) return fastTanh (x);
                return std::copysign (CurvePoints::eval (custom, juce::jmin (1.0f, std::abs (x))) + juce::jmax (0.0f, std::abs (x) - 1.0f) * 0.05f, x);
            default: return fastTanh (x);
        }
    }
    static float curve (float x, const float* p, const float* custom)
    {
        const int cv = juce::jlimit (0, kCurves - 1, ci (p, 0));
        const float g = dbToGain (p[1]), b = p[2] * 0.5f, m = clamp01 (p[6]);
        auto f = [&] (float v) { const float a = shape (cv, v, custom); return m > 0.001f ? lerp (a, shape ((cv + 1) % kCurves, v, custom), m) : a; };
        return (f (x * g + b) - f (b)) * dbToGain (p[5]) * 0.8f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        preL.setCutoff (c.p[3], sr); preR.setCutoff (c.p[3], sr);
        postL.setCutoff (c.p[4], sr); postR.setCutoff (c.p[4], sr);
        const bool pre = c.p[3] < 19000.0f, post = c.p[4] < 19000.0f;
        if (pre) for (int i = 0; i < n; ++i) { L[i] = preL.lp (L[i]); R[i] = preR.lp (R[i]); }
        float p[kMaxParams]; std::copy (c.p, c.p + kMaxParams, p);
        const float* d = c.data;
        os.process (L, R, n, [&] (float& l, float& r) { l = curve (l, p, d); r = curve (r, p, d); });
        dc (L, R, n);
        if (post) for (int i = 0; i < n; ++i) { L[i] = postL.lp (L[i]); R[i] = postR.lp (R[i]); }
    }
    bool transfer (float x, const float* p, int, const float* data, float& y) const override { y = curve (x, p, data); return true; }
private:
    OnePole preL, preR, postL, postR;
};

// ------------------------------------------------------------------ Wavefolder
// Gain, Bias, Shape, Tone, Out, Stages, Sym
class Wavefolder : public DriveBase
{
public:
    void resetState() override { tl.reset(); tr.reset(); }
    static float fold (float x, int shape)
    {
        switch (shape)
        {
            case 1: { float t = (x + 1.0f) * 0.25f; t -= std::floor (t); return 1.0f - 4.0f * std::abs (t - 0.5f); }   // triangle
            case 2: { float t = (x + 1.0f) * 0.5f; t -= std::floor (t); return 2.0f * t - 1.0f; }                      // saw wrap
            case 3: return std::sin (x * kPi * 0.5f) / (1.0f + 0.08f * x * x);                                          // lockhart-ish (folds decay)
            case 4: return fastTanh (1.8f * std::sin (x * kPi * 0.5f)) / fastTanh (1.8f);                              // serge (rounded square folds)
            case 5: return std::sin (x * kPi * 0.5f / (1.0f + 0.12f * std::abs (x)));                                   // smooth (gentler folds)
            default: return std::sin (x * kPi * 0.5f);
        }
    }
    static float curve (float x, const float* p)
    {
        const int s = juce::jlimit (0, 5, ci (p, 2)), stages = juce::jlimit (1, 4, ci (p, 5));
        const float b = p[1] * 0.8f, sym = p[6];
        auto run = [&] (float v)
        {
            v = v * p[0] + b;
            v = v > 0.0f ? v * (1.0f + sym * 0.5f) : v * (1.0f - sym * 0.5f);
            for (int k = 0; k < stages; ++k) v = fold (k == 0 ? v : v * (1.0f + 0.5f * (p[0] - 1.0f) * 0.25f), s);
            return v;
        };
        return (run (x) - run (0.0f)) * dbToGain (p[4]) * 0.45f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        float p[kMaxParams]; std::copy (c.p, c.p + kMaxParams, p);
        os.process (L, R, n, [&] (float& l, float& r) { l = curve (l, p); r = curve (r, p); });
        dc (L, R, n);
        tl.setCutoff (c.p[3], sr); tr.setCutoff (c.p[3], sr);
        if (c.p[3] < 19000.0f) for (int i = 0; i < n; ++i) { L[i] = tl.lp (L[i]); R[i] = tr.lp (R[i]); }
    }
    bool transfer (float x, const float* p, int, const float*, float& y) const override { y = curve (x, p); return true; }
private:
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Wavetable drive
// Pos, Drive, Rate, Depth, Tone, Out | Smooth / Step
// The current table frame is the transfer curve: x in -1..1 reads one cycle,
// louder input wraps around it.
class WavetableDrive : public DriveBase
{
public:
    void resetState() override { lfo.reset(); tl.reset(); tr.reset(); }
    static float position (const Wavetable* t, float pos, int mode)
    {
        if (mode == 1 && t != nullptr && t->numFrames > 1)
            return std::round (pos * (float) (t->numFrames - 1)) / (float) (t->numFrames - 1);
        return pos;
    }
    // input maps around the middle of the cycle (x = 0 -> phase 0.5, +-1 -> +-quarter
    // cycle), negated: a sine frame becomes sin(pi x / 2), a saw frame a clean line,
    // a square frame a hard clipper. Louder input runs on through the cycle (folds).
    static float curve (const Wavetable* t, float x, float pos, float g, float out)
    {
        return -t->sample (pos, 0.5f + 0.25f * x * g) * out;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        const auto* t = c.table != nullptr ? c.table : Wavetable::builtIn().get();
        const float pos = position (t, clamp01 (c.p[0] + c.p[3] * 0.5f * lfo.valueAt (0, 0.0)), c.mode);
        lfo.advance (c.p[2] * n / sr);
        const float g = dbToGain (c.p[1]), out = dbToGain (c.p[5]) * 0.6f;
        os.process (L, R, n, [&] (float& l, float& r) { l = curve (t, l, pos, g, out); r = curve (t, r, pos, g, out); });
        dc (L, R, n);
        tl.setCutoff (c.p[4], sr); tr.setCutoff (c.p[4], sr);
        if (c.p[4] < 19000.0f) for (int i = 0; i < n; ++i) { L[i] = tl.lp (L[i]); R[i] = tr.lp (R[i]); }
        taps->set (1, pos);
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override
    {
        const auto* t = currentTable();
        y = curve (t, x, position (t, taps != nullptr ? taps->get (1) : p[0], mode), dbToGain (p[1]), dbToGain (p[5]) * 0.6f);
        return true;
    }
private:
    Lfo lfo;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Clipper
// Input, Ceiling, Knee | 2x / 4x
class Clipper : public FxImpl
{
public:
    void prepare (double) override { os2.prepare (GridEffect::kChunk, 1); os4.prepare (GridEffect::kChunk, 2); reset(); }
    void reset() override { os2.reset(); os4.reset(); }
    static float clip (float x, float ceil, float knee)
    {
        const float a = std::abs (x), k0 = ceil * (1.0f - 0.5f * knee);
        if (a <= k0 || knee < 0.001f) return juce::jlimit (-ceil, ceil, x);
        const float span = ceil - k0;
        return std::copysign (k0 + span * fastTanh ((a - k0) / span), x);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float g = dbToGain (c.p[0]), ceil = dbToGain (c.p[1]), knee = c.p[2];
        float over = 0.0f;
        auto fn = [&] (float& l, float& r)
        {
            l *= g; r *= g;
            over = juce::jmax (over, std::abs (l), std::abs (r));
            l = clip (l, ceil, knee); r = clip (r, ceil, knee);
        };
        if (c.mode == 1) os4.process (L, R, n, fn); else os2.process (L, R, n, fn);
        // the down-sampling filter can overshoot; keep the ceiling a ceiling
        for (int i = 0; i < n; ++i) { L[i] = juce::jlimit (-ceil, ceil, L[i]); R[i] = juce::jlimit (-ceil, ceil, R[i]); }
        taps->set (1, juce::jmax (0.0f, gainToDb (over / ceil)));
    }
    bool transfer (float x, const float* p, int, const float*, float& y) const override { y = clip (x * dbToGain (p[0]), dbToGain (p[1]), p[2]); return true; }
private:
    Oversampler os2, os4;
};

// ------------------------------------------------------------------ Bitcrusher
// Bits, Rate, Jitter, Dither, Filter
class Bitcrusher : public FxImpl
{
public:
    void prepare (double) override { rng.seed (31); reset(); }
    void reset() override { ph = 1.0; hl = hr = 0.0f; for (auto& b : aa) b.reset(); lastRate = -1.0f; }
    static float quant (float x, float bits, float dither, Rng* rng)
    {
        const float levels = std::pow (2.0f, juce::jmax (0.0f, bits - 1.0f));
        if (rng != nullptr && dither > 0.0f) x += (rng->uni() - rng->uni()) * dither / levels;
        return std::round (x * levels) / levels;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float bits = c.p[0], rate = c.p[1], jit = c.p[2], dith = c.p[3];
        const bool filt = on (c, 4);
        if (filt && std::abs (rate - lastRate) > 1.0f)
        {
            lastRate = rate;
            aa[0].setLowPass (juce::jmin (rate * 0.45f, (float) sr * 0.45f), 0.707f, sr);
            for (int k = 1; k < 4; ++k) aa[k].copyCoeffs (aa[0]);
        }
        const double inc = rate / sr;
        for (int i = 0; i < n; ++i)
        {
            float l = L[i], r = R[i];
            if (filt) { l = aa[1].process (aa[0].process (l)); r = aa[3].process (aa[2].process (r)); }
            ph += inc * (1.0 + jit * rng.bipolar() * 0.9);
            if (ph >= 1.0) { ph -= std::floor (ph); hl = l; hr = r; }
            L[i] = quant (hl, bits, dith, &rng); R[i] = quant (hr, bits, dith, &rng);
        }
    }
    bool transfer (float x, const float* p, int, const float*, float& y) const override { y = quant (x, p[0], 0.0f, nullptr); return true; }
private:
    Rng rng;
    double ph = 1.0;
    float hl = 0.0f, hr = 0.0f, lastRate = -1.0f;
    Biquad aa[4];
};

// ------------------------------------------------------------------ Multiband drive
// Low, Mid, High, Low X, High X, Out, Low Out, Mid Out, High Out | Soft / Hard   (m[2..4] band peaks)
class MultibandDrive : public DriveBase
{
public:
    void resetState() override { for (auto* x : { &a[0], &a[1], &b[0], &b[1], &ap[0], &ap[1] }) x->reset(); k1 = k2 = -1.0f; }
    static float sat (float x, bool hard) { return hard ? x / std::pow (1.0f + std::pow (std::abs (x), 4.0f), 0.25f) : fastTanh (x); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        if (std::abs (c.p[3] - k1) > 0.01f || std::abs (c.p[4] - k2) > 0.01f)
        {
            k1 = c.p[3]; k2 = juce::jmax (c.p[4], c.p[3] * 1.5f);
            for (int ch = 0; ch < 2; ++ch) { a[ch].set (k1, osSr); b[ch].set (k2, osSr); ap[ch].set (k2, osSr); }
        }
        const float g[3] = { dbToGain (c.p[0]), dbToGain (c.p[1]), dbToGain (c.p[2]) };
        float comp[3];
        for (int k = 0; k < 3; ++k) comp[k] = dbToGain (c.p[6 + k]) / juce::jmax (1.0f, std::pow (g[k], 0.7f));
        const float out = dbToGain (c.p[5]);
        const bool hard = c.mode == 1;
        float bp[3] = { 0, 0, 0 };
        os.process (L, R, n, [&] (float& l, float& r)
        {
            float* ch[2] = { &l, &r };
            for (int k = 0; k < 2; ++k)
            {
                float lo, rest, mid, hi, x1, x2;
                a[k].split (*ch[k], lo, rest);
                b[k].split (rest, mid, hi);
                ap[k].split (lo, x1, x2); lo = x1 + x2;
                bp[0] = juce::jmax (bp[0], std::abs (lo)); bp[1] = juce::jmax (bp[1], std::abs (mid)); bp[2] = juce::jmax (bp[2], std::abs (hi));
                *ch[k] = (sat (lo * g[0], hard) * comp[0] + sat (mid * g[1], hard) * comp[1] + sat (hi * g[2], hard) * comp[2]) * out;
            }
        });
        for (int k = 0; k < 3; ++k) taps->set (2 + k, juce::jmax (bp[k], taps->get (2 + k) * 0.92f));
        dc (L, R, n);
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override
    {
        const float g = dbToGain (p[1]);
        y = sat (x * g, mode == 1) / juce::jmax (1.0f, std::pow (g, 0.7f)) * dbToGain (p[5]);
        return true;
    }
private:
    LR4 a[2], b[2], ap[2];
    float k1 = -1.0f, k2 = -1.0f;
};

// ------------------------------------------------------------------ LoFi degrader
// Rate, Bits, Noise, Wow, Flutter, Tone, Drive, Crackle | Tape / Vinyl
class LoFi : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 0.05f); dr.prepare (s, 0.05f); rng.seed (1234); reset(); }
    void reset() override
    {
        dl.reset(); dr.reset(); wow.reset(); flut.reset(); for (auto& b : tone) b.reset();
        ph = 1.0; hl = hr = 0.0f; hiss.reset(); pop = 0.0f; popLp.reset(); lastTone = -1.0f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        if (std::abs (c.p[5] - lastTone) > 1.0f)
        {
            lastTone = c.p[5];
            tone[0].setLowPass (c.p[5], 0.707f, sr); for (int k = 1; k < 4; ++k) tone[k].copyCoeffs (tone[0]);
        }
        const float wowD = c.p[3] * 0.004f * (float) sr, flD = c.p[4] * 0.0004f * (float) sr;
        const float drive = 1.0f + c.p[6] * 5.0f;
        const float noise = dbToGain (c.p[2]);
        const bool vinyl = c.mode == 1;
        const float crackle = vinyl ? c.p[7] : 0.0f;
        hiss.setCutoff (vinyl ? 3000.0f : 8000.0f, sr);
        popLp.setCutoff (2500.0f, sr);
        const double inc = c.p[0] / sr;
        for (int i = 0; i < n; ++i)
        {
            dl.write (L[i]); dr.write (R[i]);
            const float m = wowD * (1.0f + wow.valueAt (0, 0.0)) + flD * (1.0f + flut.valueAt (0, 0.0)) + 2.0f;
            wow.advance (0.55 / sr); flut.advance (7.0 / sr);
            float l = dl.readCubic (m), r = dr.readCubic (m);
            l = fastTanh (l * drive) / std::sqrt (drive); r = fastTanh (r * drive) / std::sqrt (drive);
            l = tone[1].process (tone[0].process (l)); r = tone[3].process (tone[2].process (r));
            ph += inc;
            if (ph >= 1.0) { ph -= std::floor (ph); hl = l; hr = r; }
            const float levels = std::pow (2.0f, c.p[1] - 1.0f);
            l = std::round (hl * levels) / levels; r = std::round (hr * levels) / levels;
            const float h = hiss.lp (rng.bipolar()) * noise * 3.0f;
            if (crackle > 0.0f && rng.uni() < crackle * crackle * 0.0015f) pop = rng.bipolar() * (0.1f + 0.4f * rng.uni());
            const float pp = popLp.lp (pop); pop *= 0.6f;
            L[i] = l + h + pp; R[i] = r + h * 0.9f + pp;
        }
    }
private:
    DelayLine dl, dr;
    Lfo wow, flut;
    Biquad tone[4];
    Rng rng;
    double ph = 1.0;
    float hl = 0.0f, hr = 0.0f, pop = 0.0f, lastTone = -1.0f;
    OnePole hiss, popLp;
};

// ------------------------------------------------------------------ Additive drive
// Amount, Focus, Out, Odd/Even, Drive
// Shape data: levels of harmonics 2..16 (Chebyshev polynomials of the
// envelope-normalised signal give each harmonic exactly).
class Additive : public DriveBase
{
public:
    static constexpr int NH = Shape::kHarmonics;
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        for (int h = 0; h < NH; ++h) d[h] = 0.0f;
        d[0] = 0.3f; d[1] = 0.2f;
    }
    void resetState() override { hpL.reset(); hpR.reset(); el.reset(); er.reset(); lastF = -1.0f; d2l.reset(); d2r.reset(); }
    // sum over harmonics h = 2..16 of level_h * T_h(u), even harmonics' DC removed
    static float harmonics (float u, const float* lv, float amount, float oddEven)
    {
        u = juce::jlimit (-1.0f, 1.0f, u);
        float t0 = 1.0f, t1 = u, sum = 0.0f;
        for (int h = 2; h <= NH + 1; ++h)
        {
            const float t2 = 2.0f * u * t1 - t0;
            t0 = t1; t1 = t2;
            const float l = lv[h - 2];
            if (l <= 0.0001f) continue;
            const bool even = (h & 1) == 0;
            const float w = even ? 1.0f + oddEven : 1.0f - oddEven;
            const float dc = even ? ((h / 2) % 2 == 0 ? 1.0f : -1.0f) : 0.0f;   // T_h(0)
            sum += l * w * (t2 - dc);
        }
        return 0.5f * amount * sum;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        peak (L, R, n);
        if (std::abs (c.p[1] - lastF) > 0.01f) { hpL.setHighPass (c.p[1], 0.707f, osSr); hpR.copyCoeffs (hpL); lastF = c.p[1]; }
        const bool focus = c.p[1] > 21.0f;
        el.set (0.2f, 30.0f, osSr); er.set (0.2f, 30.0f, osSr);
        const float out = dbToGain (c.p[2]), amount = c.p[0], oe = c.p[3], drive = dbToGain (c.p[4]);
        float lv[NH]; std::copy (c.data, c.data + NH, lv);
        os.process (L, R, n, [&] (float& l, float& r)
        {
            const float bl = focus ? hpL.process (l) : l, br = focus ? hpR.process (r) : r;
            const float envl = juce::jmax (1.0e-4f, el.process (bl)), envr = juce::jmax (1.0e-4f, er.process (br));
            l = (l + d2l.process (envl * harmonics (bl / envl * drive, lv, amount, oe), 0.999f)) * out;
            r = (r + d2r.process (envr * harmonics (br / envr * drive, lv, amount, oe), 0.999f)) * out;
        });
    }
    bool transfer (float x, const float* p, int, const float* data, float& y) const override
    {
        y = data != nullptr ? (x + harmonics (x * dbToGain (p[4]), data, p[0], p[3])) * dbToGain (p[2]) : x;
        return true;
    }
private:
    Biquad hpL, hpR;
    EnvFollower el, er;
    DcBlocker d2l, d2r;
    float lastF = -1.0f;
};

} // namespace

std::unique_ptr<FxImpl> makeDriveFx (const juce::String& n)
{
    if (n == "Soft Clip Drive")  return std::make_unique<SoftClip>();
    if (n == "Saturation")       return std::make_unique<Saturation>();
    if (n == "Fuzz")             return std::make_unique<Fuzz>();
    if (n == "Phase Distortion") return std::make_unique<PhaseDist>();
    if (n == "Waveshaper")       return std::make_unique<Waveshaper>();
    if (n == "Wavefolder")       return std::make_unique<Wavefolder>();
    if (n == "Wavetable Drive")  return std::make_unique<WavetableDrive>();
    if (n == "Clipper")          return std::make_unique<Clipper>();
    if (n == "Bitcrusher")       return std::make_unique<Bitcrusher>();
    if (n == "Multiband Drive")  return std::make_unique<MultibandDrive>();
    if (n == "LoFi Degrader")    return std::make_unique<LoFi>();
    if (n == "Additive Drive")   return std::make_unique<Additive>();
    return nullptr;
}

} // namespace efx
