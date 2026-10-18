#include "FxCommon.h"

// Dynamics. Meter conventions (VizTaps::m):
//   m[0] gain reduction / change in dB (negative = reduction)
//   m[1] detector level in dB
//   m[2] output guard reduction (dB)
//   Multiband Dynamics: m[0..2] band gain dB, m[3..5] band level dB
//   Denoiser: m[0..23] band gain dB, m[24..47] band level dB, m[48..71] noise floor dB
// transfer(): static curve, x = input dB -> y = output dB

namespace efx
{
namespace
{

// ------------------------------------------------------------------ Compressor
// Thresh, Ratio, Attack, Release, Knee, Makeup, SC HP, Auto Gain, Guard | Peak / RMS
// (also the Sidechain Comp: Thresh, Ratio, Attack, Release, SC HP, Makeup, Guard | Int / Ext)
class Compressor : public FxImpl
{
public:
    explicit Compressor (bool sidechainVariant) : sc (sidechainVariant) {}
    void prepare (double s) override { guard.prepare (s); reset(); }
    void reset() override { hpL.reset(); hpR.reset(); gs.reset(); guard.reset(); rms = 0.0f; peakEnv = 0.0f; lastHp = -1.0f; }

    int iAuto() const { return sc ? -1 : 7; }
    int iGuard() const { return sc ? 6 : 8; }
    // automatic make-up: half the gain reduction a -12 dBFS peak would get
    float makeupDb (const float* p) const
    {
        float m = p[iMakeup()];
        if (iAuto() >= 0 && ci (p, iAuto()) == 1)
            m -= 0.5f * compCurve (-12.0f, p[iTh()], p[iRatio()], iKnee() >= 0 ? p[iKnee()] : 6.0f);
        return m;
    }

    int iTh() const { return 0; }
    int iRatio() const { return 1; }
    int iAtk() const { return 2; }
    int iRel() const { return 3; }
    int iKnee() const { return sc ? -1 : 4; }
    int iMakeup() const { return sc ? 5 : 5; }
    int iHp() const { return sc ? 4 : 6; }

    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float th = c.p[iTh()], ratio = c.p[iRatio()];
        const float knee = iKnee() >= 0 ? c.p[iKnee()] : 6.0f;
        const float makeup = dbToGain (makeupDb (c.p));
        const bool guarded = ci (c, iGuard()) == 1;
        // peak detector (instant attack, Release decay) -> gain computer -> gain
        // smoothing (Attack down, short recovery since the detector already holds)
        const float atk = msCoef (c.p[iAtk()], sr), detRel = msCoef (c.p[iRel()], sr), gRel = msCoef (3.0f, sr);
        const float hpHz = c.p[iHp()];
        if (std::abs (hpHz - lastHp) > 0.01f) { hpL.setHighPass (hpHz, 0.707f, sr); hpR.copyCoeffs (hpL); lastHp = hpHz; }
        const bool useHp = hpHz > 21.0f;
        const bool ext = sc && c.mode == 1 && c.scL != nullptr;
        const bool rmsMode = ! sc && c.mode == 1;
        const float rmsCoef = msCoef (10.0f, sr);
        float minG = 0.0f, det = -120.0f;
        for (int i = 0; i < n; ++i)
        {
            float kl = ext ? c.scL[i] : L[i], kr = ext ? c.scR[i] : R[i];
            if (useHp) { kl = hpL.process (kl); kr = hpR.process (kr); }
            float lvl;
            if (rmsMode)
            {
                rms += rmsCoef * (0.5f * (kl * kl + kr * kr) - rms);
                lvl = std::sqrt (juce::jmax (0.0f, rms)) * 1.41421356f;
            }
            else
            {
                const float a = juce::jmax (std::abs (kl), std::abs (kr));
                peakEnv = a > peakEnv ? a : peakEnv + (a - peakEnv) * detRel;
                lvl = peakEnv;
            }
            const float x = gainToDb (lvl);
            const float g = gs.process (compCurve (x, th, ratio, knee), atk, rmsMode ? detRel : gRel);
            const float gl = dbToGain (g) * makeup;
            L[i] *= gl; R[i] *= gl;
            if (guarded) guard.process (L[i], R[i], 0.966f);   // -0.3 dBFS
            minG = juce::jmin (minG, g); det = juce::jmax (det, x);
        }
        taps->set (0, minG); taps->set (1, det); taps->set (2, guard.takeReduction());
    }

    bool transfer (float x, const float* p, int, const float*, float& y) const override
    {
        const float knee = iKnee() >= 0 ? p[iKnee()] : 6.0f;
        y = x + compCurve (x, p[iTh()], p[iRatio()], knee) + makeupDb (p);
        if (ci (p, iGuard()) == 1) y = juce::jmin (y, -0.3f);
        return true;
    }

private:
    bool sc;
    Biquad hpL, hpR;
    GainSmoother gs;
    ClipGuard guard;
    float rms = 0.0f, peakEnv = 0.0f, lastHp = -1.0f;
};

// ------------------------------------------------------------------ Limiter
// Input, Ceiling, Release, Hold, Link, Out | Clean / Punch
class Limiter : public FxImpl
{
public:
    void prepare (double s) override { minL.prepare ((int) (0.021 * s) + 2); minR.prepare ((int) (0.021 * s) + 2); reset(); }
    void reset() override { minL.reset(); minR.reset(); gL = gR = 1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float in = dbToGain (c.p[0]), ceil = dbToGain (c.p[1]);
        const float rel = msCoef (c.p[2], sr);
        const int hold = juce::jmax (1, (int) (c.p[3] * 0.001 * sr));
        minL.setSize (hold); minR.setSize (hold);
        const float link = c.p[4], out = dbToGain (c.p[5]);
        const bool punch = c.mode == 1;
        float maxRed = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            float l = L[i] * in, r = R[i] * in;
            if (punch)
            {
                // soft-saturate the top 4 dB so the limiter works less hard
                const float knee = ceil * 0.63f;
                auto sat = [knee, ceil] (float v)
                {
                    const float a = std::abs (v);
                    if (a <= knee) return v;
                    const float over = (a - knee) / (ceil * 1.5f - knee);
                    return std::copysign (knee + (ceil * 1.5f - knee) * fastTanh (over), v);
                };
                l = sat (l); r = sat (r);
            }
            float tl = std::abs (l) > ceil ? ceil / std::abs (l) : 1.0f;
            float tr = std::abs (r) > ceil ? ceil / std::abs (r) : 1.0f;
            const float tm = juce::jmin (tl, tr);
            tl = lerp (tl, tm, link); tr = lerp (tr, tm, link);
            const float hl = minL.push (tl), hr = minR.push (tr);
            gL = hl < gL ? hl : gL + (hl - gL) * rel;
            gR = hr < gR ? hr : gR + (hr - gR) * rel;
            L[i] = juce::jlimit (-ceil, ceil, l * gL) * out;
            R[i] = juce::jlimit (-ceil, ceil, r * gR) * out;
            maxRed = juce::jmin (maxRed, gainToDb (juce::jmin (gL, gR)));
        }
        taps->set (0, maxRed);
    }
    bool transfer (float x, const float* p, int, const float*, float& y) const override
    {
        y = juce::jmin (x + p[0], p[1]) + p[5];
        return true;
    }
private:
    SlidingMin minL, minR;
    float gL = 1.0f, gR = 1.0f;
};

// ------------------------------------------------------------------ Multiband Dynamics
// Depth, Time, Upward, Downward, Low, Mid, High, Out, In, Low X, High X
// Shape data, per band b (0 low, 1 mid, 2 high) at b*4:
//   down threshold dB, up threshold dB, down ratio, up ratio
class Ott : public FxImpl
{
public:
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        // a heavy upward + downward default
        const float v[12] = { -33.8f, -40.8f, 66.7f, 4.17f,  -30.2f, -41.8f, 66.7f, 4.17f,  -35.5f, -40.8f, 66.7f, 4.17f };
        std::copy (v, v + 12, d);
    }
    void prepare (double) override { reset(); }
    void reset() override
    {
        for (auto* x : { &x1L, &x1R, &x2L, &x2R, &apL, &apR }) x->reset();
        for (auto& e : env) e = 0.0f;
        for (auto& g : gain) g.reset();
        k1 = k2 = -1.0f;
    }
    static float bandGainDb (float lvlDb, const float* band, float depth, float up, float down)
    {
        const float dT = band[0], uT = band[1];
        const float dR = juce::jmax (1.0f, band[2]), uR = juce::jmax (1.0f, band[3]);
        float g = 0.0f;
        if (lvlDb > dT) g -= (lvlDb - dT) * (1.0f - 1.0f / dR) * down * depth;
        if (lvlDb < uT && lvlDb > -80.0f) g += juce::jmin (36.0f, (uT - lvlDb) * (1.0f - 1.0f / uR)) * up * depth;
        return g;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        if (std::abs (c.p[9] - k1) > 0.01f || std::abs (c.p[10] - k2) > 0.01f)
        {
            k1 = c.p[9]; k2 = juce::jmax (c.p[10], c.p[9] * 1.5f);
            x1L.set (k1, sr); x1R.set (k1, sr);
            for (auto* x : { &x2L, &x2R, &apL, &apR }) x->set (k2, sr);
        }
        const float depth = c.p[0], tm = c.p[1], up = c.p[2], down = c.p[3];
        const float bandOut[3] = { dbToGain (c.p[4]), dbToGain (c.p[5]), dbToGain (c.p[6]) };
        const float out = dbToGain (c.p[7]), in = dbToGain (c.p[8]);
        const float tScale = std::pow (8.0f, tm * 2.0f - 1.0f);   // 1/8x .. 8x
        const float atk = msCoef (3.0f * tScale, sr), rel = msCoef (60.0f * tScale, sr);
        const float gAtk = msCoef (1.0f * tScale, sr), gRel = msCoef (40.0f * tScale, sr);
        float lv[3] = { -120, -120, -120 }, gg[3] = { 0, 0, 0 };
        for (int i = 0; i < n; ++i)
        {
            float lo[2], mid[2], hi[2], rest[2];
            x1L.split (L[i] * in, lo[0], rest[0]); x1R.split (R[i] * in, lo[1], rest[1]);
            x2L.split (rest[0], mid[0], hi[0]); x2R.split (rest[1], mid[1], hi[1]);
            float a, b;
            apL.split (lo[0], a, b); lo[0] = a + b;
            apR.split (lo[1], a, b); lo[1] = a + b;
            float* bands[3] = { lo, mid, hi };
            float ol = 0.0f, orr = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float d = juce::jmax (std::abs (bands[k][0]), std::abs (bands[k][1]));
                env[k] += (d > env[k] ? atk : rel) * (d - env[k]);
                const float ldb = gainToDb (env[k]);
                const float g = gain[k].process (bandGainDb (ldb, c.data + k * 4, depth, up, down), gAtk, gRel);
                const float lin = dbToGain (g) * bandOut[k];
                ol += bands[k][0] * lin; orr += bands[k][1] * lin;
                lv[k] = ldb; gg[k] = g;
            }
            L[i] = ol * out; R[i] = orr * out;
        }
        for (int k = 0; k < 3; ++k) { taps->set (k, gg[k]); taps->set (3 + k, lv[k]); }
    }
    bool transfer (float x, const float* p, int, const float* data, float& y) const override
    {
        y = x + p[8] + (data != nullptr ? bandGainDb (x + p[8], data + 4, p[0], p[2], p[3]) : 0.0f) + p[7];
        return true;
    }
private:
    LR4 x1L, x1R, x2L, x2R, apL, apR;
    float env[3] {}, k1 = -1.0f, k2 = -1.0f;
    GainSmoother gain[3];
};

// ------------------------------------------------------------------ Gate
// Thresh, Range, Attack, Hold, Release, Hyst, SC HP | Gate / Duck
class Gate : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { hpL.reset(); hpR.reset(); env = 0.0f; g = 0.0f; holdLeft = 0; open = false; lastHp = -1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float th = c.p[0], range = c.p[1];
        const float floorG = range <= -79.9f ? 0.0f : dbToGain (range);
        const float atk = msCoef (c.p[2], sr), rel = msCoef (c.p[4], sr);
        const int hold = (int) (c.p[3] * 0.001f * sr);
        const float hyst = c.p[5];
        if (std::abs (c.p[6] - lastHp) > 0.01f) { hpL.setHighPass (c.p[6], 0.707f, sr); hpR.copyCoeffs (hpL); lastHp = c.p[6]; }
        const bool useHp = c.p[6] > 21.0f;
        const bool duck = c.mode == 1;
        const float envRel = msCoef (5.0f, sr);
        float det = -120.0f;
        for (int i = 0; i < n; ++i)
        {
            float kl = L[i], kr = R[i];
            if (useHp) { kl = hpL.process (kl); kr = hpR.process (kr); }
            const float a = juce::jmax (std::abs (kl), std::abs (kr));
            env = a > env ? a : env + (a - env) * envRel;
            const float ldb = gainToDb (env);
            if (ldb > th) { open = true; holdLeft = hold; }
            else if (open && ldb < th - hyst) { if (holdLeft > 0) --holdLeft; else open = false; }
            const bool pass = duck ? ! open : open;
            const float target = pass ? 1.0f : floorG;
            g += (target > g ? atk : rel) * (target - g);
            L[i] *= g; R[i] *= g;
            det = juce::jmax (det, ldb);
        }
        taps->set (0, gainToDb (g)); taps->set (1, det); taps->set (2, open ? 1.0f : 0.0f);
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override
    {
        const float range = p[1] <= -79.9f ? -100.0f : p[1];
        const bool pass = (x > p[0]) != (mode == 1);
        y = pass ? x : x + range;
        return true;
    }
private:
    Biquad hpL, hpR;
    float env = 0.0f, g = 0.0f, lastHp = -1.0f;
    int holdLeft = 0;
    bool open = false;
};

// ------------------------------------------------------------------ Multiband Gate
// X1..X4, Thresh, Depth, Attack, Hold, Release, Hyst, Out | Int / Ext
// Five bands from an LR4 tree; the lower bands get all-passes matching the
// crossovers above them, so the bands sum back flat (magnitude) when open.
// Each band gates on its own level (or the sidechain's, split the same way
// in Ext mode): open above its threshold (+ Thresh), closing Hyst dB lower
// after Hold, dropping by its range (x Depth). Shape data: per band
// threshold, range, on (Shape::kMbg*).
// Taps: m[0..4] band level dB, m[5..9] band gain dB.
class MultibandGate : public FxImpl
{
public:
    static constexpr int NB = Shape::kMbgBands;
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        for (int b = 0; b < NB; ++b) { d[b * Shape::kMbgStride] = -50.0f; d[b * Shape::kMbgStride + 1] = -60.0f; d[b * Shape::kMbgStride + 2] = 1.0f; }
    }
    void prepare (double) override { reset(); }
    void reset() override
    {
        for (auto* ch : { &main, &key }) for (auto& side : *ch) { for (auto& x : side.xo) x.reset(); for (auto& a : side.ap) a.reset(); }
        for (auto& b : band) b = Band();
        for (auto& f : lastX) f = -1.0f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        setCrossovers (c);
        const bool ext = c.mode == 1 && c.scL != nullptr;
        const float atk = msCoef (c.p[6], sr), rel = msCoef (c.p[8], sr), envRel = msCoef (5.0f, sr);
        const int hold = (int) (c.p[7] * 0.001f * sr);
        const float offs = c.p[4], depth = c.p[5], hyst = c.p[9], out = dbToGain (c.p[10]);
        float th[NB], floorG[NB];
        bool on[NB];
        for (int b = 0; b < NB; ++b)
        {
            const float* d = c.data + b * Shape::kMbgStride;
            th[b] = d[0] + offs;
            const float range = d[1] * depth;
            floorG[b] = range <= Shape::kMbgFloorDb + 0.1f ? 0.0f : dbToGain (range);
            on[b] = d[2] > 0.5f;
            band[b].peak = 0.0f;
        }
        for (int i = 0; i < n; ++i)
        {
            float bl[NB], br[NB], kl[NB], kr[NB];
            main[0].split (L[i], bl); main[1].split (R[i], br);
            if (ext) { key[0].split (c.scL[i], kl); key[1].split (c.scR[i], kr); }
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < NB; ++b)
            {
                auto& g = band[b];
                const float a = ext ? juce::jmax (std::abs (kl[b]), std::abs (kr[b])) : juce::jmax (std::abs (bl[b]), std::abs (br[b]));
                g.env = a > g.env ? a : g.env + (a - g.env) * envRel;
                g.peak = juce::jmax (g.peak, g.env);
                const float ldb = gainToDb (g.env);
                if (ldb > th[b]) { g.open = true; g.holdLeft = hold; }
                else if (g.open && ldb < th[b] - hyst) { if (g.holdLeft > 0) --g.holdLeft; else g.open = false; }
                const float target = (! on[b] || g.open) ? 1.0f : floorG[b];
                g.gain += (target > g.gain ? atk : rel) * (target - g.gain);
                ol += bl[b] * g.gain; orr += br[b] * g.gain;
            }
            L[i] = ol * out; R[i] = orr * out;
        }
        for (int b = 0; b < NB; ++b)
        {
            taps->set (b, gainToDb (band[b].peak));
            taps->set (NB + b, gainToDb (band[b].gain));
        }
    }
private:
    // one channel's 5-band tree: LR4 splits, plus all-passes for the lower bands
    struct Tree
    {
        LR4 xo[NB - 1];
        Biquad ap[6];   // band 0: AP1 AP2 AP3, band 1: AP2 AP3, band 2: AP3
        void set (const float* f, double sr)
        {
            for (int k = 0; k < NB - 1; ++k) xo[k].set (f[k], sr);
            ap[0].setAllPass (f[1], 0.70710678f, sr); ap[1].setAllPass (f[2], 0.70710678f, sr); ap[2].setAllPass (f[3], 0.70710678f, sr);
            ap[3].setAllPass (f[2], 0.70710678f, sr); ap[4].setAllPass (f[3], 0.70710678f, sr);
            ap[5].setAllPass (f[3], 0.70710678f, sr);
        }
        inline void split (float x, float* out)
        {
            float lo, rest = x;
            xo[0].split (rest, lo, rest); out[0] = ap[2].process (ap[1].process (ap[0].process (lo)));
            xo[1].split (rest, lo, rest); out[1] = ap[4].process (ap[3].process (lo));
            xo[2].split (rest, lo, rest); out[2] = ap[5].process (lo);
            xo[3].split (rest, out[3], out[4]);
        }
    };
    struct Band { float env = 0.0f, gain = 1.0f, peak = 0.0f; int holdLeft = 0; bool open = true; };
    void setCrossovers (const FxContext& c)
    {
        float f[NB - 1];
        bool changed = false;
        for (int k = 0; k < NB - 1; ++k) { f[k] = Shape::mbgCrossover (c.p, k); changed |= std::abs (f[k] - lastX[k]) > 0.01f; }
        if (! changed) return;
        for (int k = 0; k < NB - 1; ++k) lastX[k] = f[k];
        for (auto* ch : { &main, &key }) for (auto& side : *ch) side.set (f, sr);
    }
    std::array<Tree, 2> main, key;
    Band band[NB];
    float lastX[NB - 1] { -1.0f, -1.0f, -1.0f, -1.0f };
};

// ------------------------------------------------------------------ Denoiser
// Reduce, Sens, Attack, Release, HF Bias, Smooth, Learn | Adaptive / Learned
// 24 bands. Each band tracks its noise floor (minimum statistics, or an
// average captured while Learn is on) and is turned down when its level
// falls within Sens dB of that floor. Gains are smoothed across neighbouring
// bands to avoid "musical noise".
class Denoiser : public FxImpl
{
public:
    static constexpr int NB = 24;
    void prepare (double s) override { spL.prepare (s, NB, 80.0f, 14000.0f); spR.prepare (s, NB, 80.0f, 14000.0f); reset(); }
    void reset() override
    {
        spL.reset(); spR.reset();
        for (auto& e : env) e = 1.0e-6f;
        for (auto& f : floorDb) f = -90.0f;
        for (auto& g : gs) g.reset();
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float maxRed = c.p[0], sens = c.p[1];
        const float atk = msCoef (c.p[2], sr), rel = msCoef (c.p[3], sr);
        const float bias = c.p[4], smooth = c.p[5];
        const bool learned = c.mode == 1, learning = learned && on (c, 6);
        const float envA = msCoef (3.0f, sr), envR = msCoef (40.0f, sr);
        const float rise = 3.0f / (float) sr;                    // the floor creeps up 3 dB/s
        const float fall = msCoef (80.0f, sr), learnCoef = msCoef (400.0f, sr);
        float lvl[NB], target[NB], last[NB] {};
        for (int i = 0; i < n; ++i)
        {
            float bl[NB], br[NB];
            spL.split (L[i], bl); spR.split (R[i], br);
            for (int b = 0; b < NB; ++b)
            {
                const float a = 0.5f * (std::abs (bl[b]) + std::abs (br[b]));
                env[b] += (a > env[b] ? envA : envR) * (a - env[b]);
                lvl[b] = gainToDb (env[b]);
                if (! learned)
                {
                    if (lvl[b] < floorDb[b]) floorDb[b] += (lvl[b] - floorDb[b]) * fall;
                    else floorDb[b] += rise;
                }
                else if (learning) floorDb[b] += (lvl[b] - floorDb[b]) * learnCoef;
                const float th = floorDb[b] + sens + bias * 6.0f * (2.0f * (float) b / (NB - 1) - 1.0f);
                const float under = th - lvl[b];
                target[b] = under > 0.0f ? -juce::jmin (maxRed, under * 2.0f) : 0.0f;
            }
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < NB; ++b)
            {
                const float nb = 0.5f * (target[juce::jmax (0, b - 1)] + target[juce::jmin (NB - 1, b + 1)]);
                const float t = lerp (target[b], nb, smooth * 0.6f);
                const float gdb = gs[b].process (t, rel, atk);   // opening uses attack
                const float g = dbToGain (gdb);
                ol += bl[b] * g; orr += br[b] * g;
                last[b] = gdb;
            }
            L[i] = ol; R[i] = orr;
        }
        for (int b = 0; b < NB; ++b) { taps->set (b, last[b]); taps->set (24 + b, lvl[b]); taps->set (48 + b, floorDb[b]); }
    }
private:
    BandSplitter<NB> spL, spR;
    float env[NB] {}, floorDb[NB] {};
    GainSmoother gs[NB];
};

// ------------------------------------------------------------------ Expander
// Thresh, Ratio, Range, Attack, Release, Knee | Down / Up
class Expander : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { gs.reset(); env = 0.0f; }
    static float curve (float x, float th, float ratio, float range, float knee, bool up)
    {
        const float k = juce::jmax (0.01f, knee);
        if (! up)
        {
            float under = th - x;   // > 0 below threshold
            float e;
            if (under <= -k * 0.5f) e = 0.0f;
            else if (under < k * 0.5f) { const float t = under + k * 0.5f; e = t * t / (2.0f * k); }
            else e = under;
            return -juce::jmin (range, e * (ratio - 1.0f));
        }
        float over = x - th;
        float e;
        if (over <= -k * 0.5f) e = 0.0f;
        else if (over < k * 0.5f) { const float t = over + k * 0.5f; e = t * t / (2.0f * k); }
        else e = over;
        return juce::jmin (range, e * (ratio - 1.0f) * 0.5f);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float th = c.p[0], ratio = c.p[1], range = c.p[2], knee = c.p[5];
        const float atk = msCoef (c.p[3], sr), rel = msCoef (c.p[4], sr);
        const bool up = c.mode == 1;
        const float envR = msCoef (20.0f, sr);
        float last = 0.0f, det = -120.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = juce::jmax (std::abs (L[i]), std::abs (R[i]));
            env = a > env ? a : env + (a - env) * envR;
            const float x = gainToDb (env);
            const float target = curve (x, th, ratio, range, knee, up);
            // gain rising = opening = attack
            last = gs.process (target, rel, atk);
            const float g = dbToGain (last);
            L[i] *= g; R[i] *= g;
            det = juce::jmax (det, x);
        }
        taps->set (0, last); taps->set (1, det);
    }
    bool transfer (float x, const float* p, int mode, const float*, float& y) const override
    {
        y = x + curve (x, p[0], p[1], p[2], p[5], mode == 1);
        return true;
    }
private:
    GainSmoother gs;
    float env = 0.0f;
};

// ------------------------------------------------------------------ Squasher
// Amount, Punch, Tone, Release, Drive, Out, Target | Fat / Flat
class Squasher : public FxImpl
{
public:
    void prepare (double s) override { fatL.setLowShelf (120.0f, 0.7f, 3.0f, s); fatR.copyCoeffs (fatL); reset(); }
    void reset() override { env = 0.0f; gs.reset(); tilt[0].reset(); tilt[1].reset(); fatL.reset(); fatR.reset(); }
    static float gainFor (float ldb, float amt, float target)
    {
        if (ldb < -65.0f) return 0.0f;
        return juce::jlimit (-30.0f, 24.0f, (target - ldb) * amt * 0.92f);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float amt = c.p[0], punch = c.p[1];
        const float atk = msCoef (0.5f + punch * 30.0f, sr), rel = msCoef (c.p[3], sr);
        const float drive = 1.0f + c.p[4] * 5.0f;
        const float out = dbToGain (c.p[5]);
        tilt[0].set (c.p[2], sr); tilt[1].set (c.p[2], sr);
        const bool fat = c.mode == 0;
        float last = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = std::sqrt (0.5f * (L[i] * L[i] + R[i] * R[i])) * 1.41421356f;
            env += (a > env ? atk : rel) * (a - env);
            last = gs.process (gainFor (gainToDb (env), amt, c.p[6]), msCoef (2.0f, sr), msCoef (20.0f, sr));
            const float g = dbToGain (last);
            float l = L[i] * g, r = R[i] * g;
            if (fat) { l = fatL.process (l); r = fatR.process (r); }
            l = fastTanh (l * drive) / std::sqrt (drive);
            r = fastTanh (r * drive) / std::sqrt (drive);
            L[i] = tilt[0].process (l) * out; R[i] = tilt[1].process (r) * out;
        }
        taps->set (0, last);
    }
    bool transfer (float x, const float* p, int, const float*, float& y) const override
    {
        y = x + gainFor (x, p[0], p[6]) + p[5];
        return true;
    }
private:
    float env = 0.0f;
    GainSmoother gs;
    Tilt tilt[2];
    Biquad fatL, fatR;
};

} // namespace

std::unique_ptr<FxImpl> makeDynamicsFx (const juce::String& n)
{
    if (n == "Compressor")     return std::make_unique<Compressor> (false);
    if (n == "Sidechain Comp") return std::make_unique<Compressor> (true);
    if (n == "Limiter")        return std::make_unique<Limiter>();
    if (n == "Multiband Dynamics")  return std::make_unique<Ott>();
    if (n == "Gate")           return std::make_unique<Gate>();
    if (n == "Multiband Gate") return std::make_unique<MultibandGate>();
    if (n == "Denoiser")       return std::make_unique<Denoiser>();
    if (n == "Expander")       return std::make_unique<Expander>();
    if (n == "Squasher")       return std::make_unique<Squasher>();
    return nullptr;
}

} // namespace efx
