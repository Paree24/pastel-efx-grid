#include "FxCommon.h"
#include "../HarmRes.h"

// Stereo + Pitch. Meter conventions (VizTaps::m):
//   Stereo Width / Haas / Mono Bass: m[0] output correlation -1..1
//   Pitch Correction: m[0] detected MIDI, m[1] target MIDI, m[2] correction (semis)
//   Harmonizer: m[0] detected MIDI, m[1], m[2] voice offsets (semis)
//   Vocoder: m[0..31] band envelopes (0..1)
//   Pitch Shifter / Frequency Shifter: m[0] effective shift

namespace efx
{
namespace
{

struct CorrMeter
{
    float lr = 0.0f, ll = 0.0f, rr = 0.0f;
    void add (float l, float r) { lr += l * r; ll += l * l; rr += r * r; }
    void publish (VizTaps* t)
    {
        const float d = std::sqrt (ll * rr);
        if (d > 1.0e-9f) { const float c = lr / d; t->set (0, t->get (0) + (c - t->get (0)) * 0.3f); }
        lr = ll = rr = 0.0f;
    }
};

// ------------------------------------------------------------------ Stereo Width
// Width, Spread, Bass Mono, Balance, Out
class StereoWidth : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { xo.reset(); for (auto& a : ap1) a.reset(); for (auto& a : ap2) a.reset(); lastX = -1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float width = c.p[0], spread = c.p[1], bassHz = c.p[2];
        if (std::abs (bassHz - lastX) > 0.01f) { xo.set (bassHz, sr); lastX = bassHz; }
        const bool bassMono = bassHz > 21.0f;
        const float bal = c.p[3];
        const float gl = bal > 0.0f ? 1.0f - bal : 1.0f, gr = bal < 0.0f ? 1.0f + bal : 1.0f;
        const float out = dbToGain (c.p[4]);
        static constexpr float c1[4] = { 0.31f, -0.52f, 0.67f, -0.18f }, c2[4] = { -0.41f, 0.23f, -0.74f, 0.58f };
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            float s = 0.5f * (L[i] - R[i]);
            if (bassMono) { float lo, hi; xo.split (s, lo, hi); s = hi; }
            s *= width;
            if (spread > 0.001f)
            {
                float a = m, b = m;
                for (int k = 0; k < 4; ++k) { a = ap1[k].process (a, c1[k]); b = ap2[k].process (b, c2[k]); }
                s += 0.5f * spread * (a - b);
            }
            L[i] = (m + s) * gl * out; R[i] = (m - s) * gr * out;
            cm.add (L[i], R[i]);
        }
        cm.publish (taps);
    }
private:
    LR4 xo;
    Allpass1 ap1[4], ap2[4];
    float lastX = -1.0f;
    CorrMeter cm;
};

// ------------------------------------------------------------------ Haas
// Delay, Level, Lowcut, Out | Right / Left
class Haas : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 0.05f); reset(); }
    void reset() override { dl.reset(); xo.reset(); lastX = -1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        if (std::abs (c.p[2] - lastX) > 0.01f) { xo.set (c.p[2], sr); lastX = c.p[2]; }
        const float d = juce::jmax (1.0f, c.p[0] * 0.001f * (float) sr), level = c.p[1], out = dbToGain (c.p[3]);
        const bool right = c.mode == 0;
        for (int i = 0; i < n; ++i)
        {
            float& x = right ? R[i] : L[i];
            float lo, hi;
            xo.split (x, lo, hi);
            dl.write (hi);
            x = lo + dl.read (d) * level + hi * (1.0f - level);
            L[i] *= out; R[i] *= out;
            cm.add (L[i], R[i]);
        }
        cm.publish (taps);
    }
private:
    DelayLine dl;
    LR4 xo;
    float lastX = -1.0f;
    CorrMeter cm;
};

// ------------------------------------------------------------------ Mono Bass
// Xover, Low Width, Low Gain, High Width, Listen | 12 dB / 24 dB
class MonoBass : public FxImpl
{
public:
    void prepare (double s) override { sl.prepare (s); sr2.prepare (s); reset(); }
    void reset() override { xl.reset(); xr.reset(); sl.reset(); sr2.reset(); lastX = -1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float fx = c.p[0];
        if (std::abs (fx - lastX) > 0.01f) { xl.set (fx, sr); xr.set (fx, sr); lastX = fx; }
        const float lw = c.p[1], lg = dbToGain (c.p[2]), hw = c.p[3];
        const int listen = ci (c, 4);
        for (int i = 0; i < n; ++i)
        {
            float lol, hil, lor, hir;
            if (c.mode == 1) { xl.split (L[i], lol, hil); xr.split (R[i], lor, hir); }
            else
            {
                float b, h;
                sl.processQ (L[i], fx, 0.5f, lol, b, h); hil = L[i] - lol;
                sr2.processQ (R[i], fx, 0.5f, lor, b, h); hir = R[i] - lor;
            }
            applyWidth (lol, lor, lw);
            applyWidth (hil, hir, hw);
            lol *= lg; lor *= lg;
            if (listen == 1) { hil = hir = 0.0f; }
            else if (listen == 2) { lol = lor = 0.0f; }
            L[i] = lol + hil; R[i] = lor + hir;
            cm.add (L[i], R[i]);
        }
        cm.publish (taps);
    }
private:
    LR4 xl, xr;
    Svf sl, sr2;
    float lastX = -1.0f;
    CorrMeter cm;
};

// ------------------------------------------------------------------ Frequency Shifter
// Shift, Feedback, Spread, Tone
class FreqShift : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { fl.reset(); fr.reset(); tl.reset(); tr.reset(); fbl = fbr = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float sh = c.p[0], fb = c.p[1];
        const float shR = sh * (1.0f - 2.0f * c.p[2]);
        tl.setCutoff (c.p[3], sr); tr.setCutoff (c.p[3], sr);
        for (int i = 0; i < n; ++i)
        {
            const float yl = fl.process (L[i] + fbl * fb, sh, sr);
            const float yr = fr.process (R[i] + fbr * fb, shR, sr);
            fbl = loopSat (tl.lp (yl)); fbr = loopSat (tr.lp (yr));
            L[i] = fbl; R[i] = fbr;
        }
        taps->set (0, sh);
    }
private:
    FreqShifter fl, fr;
    OnePole tl, tr;
    float fbl = 0.0f, fbr = 0.0f;
};

// ------------------------------------------------------------------ Pitch Shifter
// Semis, Fine, Window, Feedback, Delay, Detune
class PitchShift : public FxImpl
{
public:
    void prepare (double s) override { pl.prepare (s); pr.prepare (s); dl.prepare (s, 0.55f); dr.prepare (s, 0.55f); reset(); }
    void reset() override { pl.reset(); pr.reset(); dl.reset(); dr.reset(); fbl = fbr = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float semis = c.p[0] + c.p[1] / 100.0f;
        const float rl = semisToRatio (semis - c.p[5] / 100.0f), rr = semisToRatio (semis + c.p[5] / 100.0f);
        const float win = c.p[2] * 0.001f * (float) sr;
        const float fb = c.p[3];
        const float d = c.p[4] * 0.001f * (float) sr;
        const bool useDelay = d >= 1.0f;
        for (int i = 0; i < n; ++i)
        {
            float il = L[i] + fbl * fb, ir = R[i] + fbr * fb;
            if (useDelay) { dl.write (il); dr.write (ir); il = dl.read (d); ir = dr.read (d); }
            const float yl = pl.process (il, rl, win), yr = pr.process (ir, rr, win);
            fbl = loopSat (yl); fbr = loopSat (yr);
            L[i] = yl; R[i] = yr;
        }
        taps->set (0, semis);
    }
private:
    PitchShifter pl, pr;
    DelayLine dl, dr;
    float fbl = 0.0f, fbr = 0.0f;
};

// scales (semitones from the key)
const int kScaleChrom[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
const int kScaleMajor[7] = { 0,2,4,5,7,9,11 };
const int kScaleMinor[7] = { 0,2,3,5,7,8,10 };
const int kScalePenta[5] = { 0,2,4,7,9 };
const int kScaleBlues[6] = { 0,3,5,6,7,10 };

struct Scale { const int* s; int n; };
Scale scaleFor (int idx)
{
    switch (idx)
    {
        case 1: return { kScaleMajor, 7 };
        case 2: return { kScaleMinor, 7 };
        case 3: return { kScalePenta, 5 };
        case 4: return { kScaleBlues, 6 };
        default: return { kScaleChrom, 12 };
    }
}

// nearest scale note (MIDI) to a fractional MIDI pitch
float nearestInScale (float midi, int key, Scale sc)
{
    float best = midi, bestD = 1.0e9f;
    const int base = (int) std::floor (midi) - 12;
    for (int m = base; m <= base + 24; ++m)
    {
        const int rel = ((m - key) % 12 + 12) % 12;
        bool in = false;
        for (int i = 0; i < sc.n; ++i) if (sc.s[i] == rel) { in = true; break; }
        if (! in) continue;
        const float dd = std::abs ((float) m - midi);
        if (dd < bestD) { bestD = dd; best = (float) m; }
    }
    return best;
}

// ------------------------------------------------------------------ Pitch Correction
// Key, Scale, Speed, Amount, Humanize, Detune
class PitchCorrection : public FxImpl
{
public:
    void prepare (double s) override { pt.prepare (s); pl.prepare (s); pr.prepare (s); reset(); }
    void reset() override { pt.reset(); pl.reset(); pr.reset(); corr = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int key = ci (c, 0);
        const Scale sc = scaleFor (ci (c, 1));
        const float amount = c.p[3], human = c.p[4], detune = c.p[5] / 100.0f;
        float det = 0.0f, tgt = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            pt.push (0.5f * (L[i] + R[i]));
            const bool voiced = pt.confidence > 0.6f && pt.periodSamples > 0.0f;
            float target = 0.0f;
            if (voiced)
            {
                det = freqToMidi (pt.hz());
                tgt = nearestInScale (det, key, sc);
                target = (tgt - det + detune) * amount;
            }
            const float speed = c.p[2] * (std::abs (target - corr) < 0.4f ? 1.0f + human * 6.0f : 1.0f);
            corr += (target - corr) * msCoef (speed, sr);
            const float ratio = semisToRatio (corr);
            const float P = voiced ? pt.periodSamples : 0.0f;
            L[i] = pl.process (L[i], P, ratio, 1.0f);
            R[i] = pr.process (R[i], P, ratio, 1.0f);
        }
        taps->set (0, det); taps->set (1, tgt); taps->set (2, corr);
    }
private:
    PitchTracker pt;
    Psola pl, pr;
    float corr = 0.0f;
};

// ------------------------------------------------------------------ Harmonizer
// Key, Scale, Voice 1, Voice 2, Human, Delay, Spread, Level | Diatonic / Chromatic
class Harmonizer : public FxImpl
{
public:
    void prepare (double s) override
    {
        pt.prepare (s); for (auto& v : voice) v.prepare (s);
        for (auto& d : dly) d.prepare (s, 0.15f);
        rng.seed (99);
        reset();
    }
    void reset() override { pt.reset(); for (auto& v : voice) v.reset(); for (auto& d : dly) d.reset(); for (auto& h : drift) h = 0.0f; detected = 60.0f; }
    // interval label index -> (diatonic steps, chromatic semis)
    static void interval (int idx, int& steps, int& semis)
    {
        static const int st[9] = { -7, -4, -3, -2, 2, 3, 4, 5, 7 };
        static const int se[9] = { -12, -7, -5, -4, 4, 5, 7, 9, 12 };
        idx = juce::jlimit (0, 8, idx);
        steps = st[idx]; semis = se[idx];
    }
    static float offsetFor (float midi, int key, bool minor, int steps)
    {
        const Scale sc = minor ? Scale { kScaleMinor, 7 } : Scale { kScaleMajor, 7 };
        const float note = nearestInScale (midi, key, sc);
        const int rel = (((int) note - key) % 12 + 12) % 12;
        int deg = 0;
        for (int i = 0; i < 7; ++i) if (sc.s[i] == rel) deg = i;
        const int target = deg + steps;
        const int oct = (int) std::floor ((float) target / 7.0f);
        const int tdeg = target - oct * 7;
        return (float) (sc.s[tdeg] - sc.s[deg] + oct * 12);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int key = ci (c, 0);
        const bool minor = ci (c, 1) == 1, diatonic = c.mode == 0;
        const int v1 = ci (c, 2), v2 = ci (c, 3) - 1;   // v2 index -1 = off
        const float human = c.p[4], spread = c.p[6], level = dbToGain (c.p[7]);
        const float baseDelay = c.p[5] * 0.001f * (float) sr;
        float off[2] = { 0, 0 };
        float gl[2], gr[2];
        panGains (-spread, gl[0], gr[0]);
        panGains (spread, gl[1], gr[1]);
        if (rng.uni() < 0.05f) for (auto& t : driftT) t = rng.bipolar();
        for (int v = 0; v < 2; ++v) drift[v] += (driftT[v] - drift[v]) * 0.02f;
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            pt.push (m);
            const bool voiced = pt.confidence > 0.5f && pt.periodSamples > 0.0f;
            if (voiced) detected += (freqToMidi (pt.hz()) - detected) * 0.01f;
            float ol = 0.0f, orr = 0.0f;
            for (int v = 0; v < 2; ++v)
            {
                const int idx = v == 0 ? v1 : v2;
                if (idx < 0) continue;
                int steps, semis;
                interval (idx, steps, semis);
                off[v] = diatonic ? offsetFor (detected, key, minor, steps) : (float) semis;
                const float cents = human * 18.0f * drift[v];
                const float ratio = semisToRatio (off[v] + cents / 100.0f);
                dly[v].write (m);
                const float dIn = dly[v].read (juce::jmax (1.0f, baseDelay * (1.0f + 0.3f * (float) v) * (1.0f + human * 0.2f * drift[v])));
                const float y = voice[v].process (dIn, voiced ? pt.periodSamples : 0.0f, ratio, 1.0f);
                ol += y * gl[v]; orr += y * gr[v];
            }
            L[i] = ol * level; R[i] = orr * level;
        }
        taps->set (0, detected); taps->set (1, off[0]); taps->set (2, off[1]);
    }
private:
    PitchTracker pt;
    Psola voice[2];
    DelayLine dly[2];
    Rng rng;
    float drift[2] {}, driftT[2] {}, detected = 60.0f;
};

// ------------------------------------------------------------------ Vocoder
// Bands, Pitch, Formant, Attack, Release, Q, Sibilance | Saw / Noise / Ext
class Vocoder : public FxImpl
{
public:
    static constexpr int kMax = 32;
    static constexpr int kBands[4] = { 8, 16, 24, 32 };
    void prepare (double s) override { rng.seed (5); sibHp.setHighPass (5000.0f, 0.707f, s); reset(); }
    void reset() override
    {
        for (auto& b : mod) b.reset(); for (auto& b : carL) b.reset(); for (auto& b : carR) b.reset();
        for (auto& e : env) e = 0.0f;
        ph1 = ph2 = 0.0; key = -1.0f; sibHp.reset();
    }
    static float polyBlep (double t, double dt)
    {
        if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }
    void design (int nb, float formant, float q)
    {
        const float lo = 90.0f, hi = juce::jmin (11000.0f, (float) sr * 0.42f);
        const float spacing = std::log2 (hi / lo) / (float) (nb - 1);
        const float t = std::pow (2.0f, spacing * 0.5f);
        const float qBase = t / (t * t - 1.0f);
        const float Q = qBase * (0.6f + 1.8f * q);
        const float fs = semisToRatio (formant);
        for (int b = 0; b < nb; ++b)
        {
            const float f = lo * std::pow (2.0f, spacing * (float) b);
            mod[b].setBandPass (f, Q, sr);
            carL[b].setBandPass (juce::jmin ((float) sr * 0.45f, f * fs), Q, sr);
            carR[b].copyCoeffs (carL[b]);
        }
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int nb = kBands[juce::jlimit (0, 3, ci (c, 0))];
        const float k = (float) nb + c.p[2] * 100.0f + c.p[5] * 10000.0f;
        if (k != key) { key = k; design (nb, c.p[2], c.p[5]); }
        const float atk = msCoef (c.p[3], sr), rel = msCoef (c.p[4], sr);
        const float sib = c.p[6];
        const double f0 = midiToFreq (c.p[1]);
        const double dt1 = f0 / sr, dt2 = f0 * 1.006 / sr;
        const float norm = 2.5f / std::sqrt ((float) nb) * std::sqrt (c.p[5] * 2.0f + 0.6f);
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            float cl, cr;
            if (c.mode == 2 && c.scL != nullptr) { cl = c.scL[i]; cr = c.scR[i]; }
            else if (c.mode == 1) { cl = rng.bipolar() * 0.7f; cr = rng.bipolar() * 0.7f; }
            else
            {
                ph1 += dt1; ph1 -= std::floor (ph1);
                ph2 += dt2; ph2 -= std::floor (ph2);
                const float s1 = (float) (2.0 * ph1 - 1.0) - polyBlep (ph1, dt1);
                const float s2 = (float) (2.0 * ph2 - 1.0) - polyBlep (ph2, dt2);
                cl = 0.6f * (s1 + 0.6f * s2); cr = 0.6f * (s2 + 0.6f * s1);
            }
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < nb; ++b)
            {
                const float a = std::abs (mod[b].process (m));
                env[b] += (a > env[b] ? atk : rel) * (a - env[b]);
                ol += carL[b].process (cl) * env[b];
                orr += carR[b].process (cr) * env[b];
            }
            const float s = sibHp.process (m) * sib;
            L[i] = ol * norm * 4.0f + s; R[i] = orr * norm * 4.0f + s;
        }
        for (int b = 0; b < juce::jmin (nb, VizTaps::kMeters); ++b) taps->set (b, clamp01 (env[b] * 6.0f));
        taps->set (VizTaps::kMeters - 1, (float) nb);
    }
private:
    Biquad mod[kMax], carL[kMax], carR[kMax], sibHp;
    float env[kMax] {};
    double ph1 = 0.0, ph2 = 0.0;
    float key = -1.0f;
    Rng rng;
};

// ------------------------------------------------------------------ Resonator
// Root, Chord, Decay, Bright, Spread
class Resonator : public FxImpl
{
public:
    static constexpr int kChords[8][4] = { { 0, 0, 0, 0 }, { 0, 12, 0, 12 }, { 0, 7, 12, 19 }, { 0, 4, 7, 12 },
                                           { 0, 3, 7, 12 }, { 0, 5, 7, 12 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 } };
    void prepare (double s) override { for (auto& d : dl) d.prepare (s, 0.05f); for (auto& d : dr) d.prepare (s, 0.05f); reset(); }
    void reset() override { for (auto& d : dl) d.reset(); for (auto& d : dr) d.reset(); for (auto& f : lpl) f.reset(); for (auto& f : lpr) f.reset(); dcl.reset(); dcr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int chord = juce::jlimit (0, 7, ci (c, 1));
        const float decay = c.p[2];
        const float damp = 1000.0f * std::pow (16.0f, c.p[3]);
        const float spread = c.p[4];
        float dlen[4], drlen[4], gl[4], gr[4];
        for (int v = 0; v < 4; ++v)
        {
            const float f = midiToFreq (c.p[0] + (float) kChords[chord][v]);
            const float fR = f * std::pow (2.0f, spread * 8.0f / 1200.0f * (v % 2 ? 1.0f : -1.0f));
            dlen[v] = (float) sr / f; drlen[v] = (float) sr / fR;
            gl[v] = std::pow (10.0f, -3.0f * (dlen[v] / (float) sr) / decay);
            gr[v] = std::pow (10.0f, -3.0f * (drlen[v] / (float) sr) / decay);
            lpl[v].setCutoff (damp, sr); lpr[v].setCutoff (damp, sr);
        }
        const float exc = 0.25f;
        for (int i = 0; i < n; ++i)
        {
            const float il = dcl.process (L[i]) * exc, ir = dcr.process (R[i]) * exc;
            float ol = 0.0f, orr = 0.0f;
            for (int v = 0; v < 4; ++v)
            {
                const float yl = dl[v].read (dlen[v]), yr = dr[v].read (drlen[v]);
                dl[v].write (il + lpl[v].lp (yl) * gl[v]);
                dr[v].write (ir + lpr[v].lp (yr) * gr[v]);
                const float pan = spread * (v % 2 ? 0.5f : -0.5f);
                ol += yl * (1.0f - juce::jmax (0.0f, pan)); orr += yr * (1.0f + juce::jmin (0.0f, pan));
            }
            L[i] = loopSat (ol * 0.5f); R[i] = loopSat (orr * 0.5f);
        }
    }
private:
    DelayLine dl[4], dr[4];
    OnePole lpl[4], lpr[4];
    DcBlocker dcl, dcr;
};

// ------------------------------------------------------------------ Harmonic Resonator
// Root, Scale, Chord, Low, High, Reso, Tilt, Drive, Spread, Out | Scale / Chord / Custom
// Harmonic filtering: a bank of band-pass resonators (0 dB peak), one on
// every note of the pitch set between Low and High, so the input only comes
// through at those pitches and their octaves. Drive saturates first, so a
// plain bass grows harmonics for the bank to pick from ("colour bass").
// Spread detunes the two channels apart. Shape data [0..11]: the Custom set.
// Arp (Midifier's modes) steps through the set in tempo: only the current
// note's resonators are fed, the others ring out on their own (like strings
// left to sustain), for Gate of each step; Swing delays the off-beat steps.
// Double precision: narrow low-frequency band-passes drift in float.
class HarmResonator : public FxImpl
{
public:
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        // Custom starts as C major
        static const float major[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
        for (int k = 0; k < 12; ++k) d[k] = major[k];
    }
    void prepare (double) override { reset(); }
    void reset() override
    {
        for (auto& r : resL) r.reset();
        for (auto& r : resR) r.reset();
        for (auto& g : feed) g = 1.0f;
        count = 0; lastKey = -1.0e9; freeBeat = 0.0; lastStep = -1; curPc = -1;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        rebuildIfNeeded (c);
        const float drive = c.p[7];
        const float k = 1.0f + drive * 11.0f, norm = 1.0f / fastTanh (k);
        const float out = dbToGain (c.p[9]);
        const int arp = juce::jlimit (0, 6, ci (c, 10));
        int pool[12] {}, poolN = 0;
        double stepBeats = 0.25;
        if (arp != harmres::kArpOff)
        {
            poolN = harmres::arpPool (c.p, c.mode, c.data, arp, pool);
            stepBeats = divBeatsOf (*def, c.p, 11);
        }
        const float gateLen = c.p[12], swing = c.p[13];
        const double bps = c.bpm / 60.0 / sr;   // beats per sample
        const float feedCoef = msCoef (2.0f, sr);
        for (int i = 0; i < n; ++i)
        {
            double xl = L[i], xr = R[i];
            if (drive > 0.001f) { xl = fastTanh ((float) xl * k) * norm; xr = fastTanh ((float) xr * k) * norm; }
            if (arp != harmres::kArpOff && poolN > 0)
            {
                // the host's beat while playing, a free-running one when stopped
                const double beat = c.playing ? c.ppq + c.beatsPerSample * i : (freeBeat += bps);
                // swing: steps come in pairs, the first one longer
                const double pair = 2.0 * stepBeats, inPair = beat - std::floor (beat / pair) * pair;
                const double firstLen = stepBeats * (1.0 + swing);
                const long long step = (long long) std::floor (beat / pair) * 2 + (inPair < firstLen ? 0 : 1);
                const double phase = inPair < firstLen ? inPair / firstLen : (inPair - firstLen) / (pair - firstLen);
                if (step != lastStep)
                {
                    lastStep = step;
                    curPc = arp == harmres::kArpRandom ? pool[rng.nextInt (poolN)] : harmres::arpNote (pool, poolN, arp, step);
                }
                const bool open = phase < gateLen;
                for (int r = 0; r < count; ++r)
                    feed[r] += ((open && pcOf[r] == curPc ? 1.0f : 0.0f) - feed[r]) * feedCoef;
            }
            else
                for (int r = 0; r < count; ++r) feed[r] += (1.0f - feed[r]) * feedCoef;
            double yl = 0.0, yr = 0.0;
            for (int r = 0; r < count; ++r)
            {
                yl += resL[r].process (xl * feed[r]) * gain[r];
                yr += resR[r].process (xr * feed[r]) * gain[r];
            }
            L[i] = loopSat ((float) yl) * out;
            R[i] = loopSat ((float) yr) * out;
        }
        taps->set (0, (float) count);
        taps->set (1, arp != harmres::kArpOff ? (float) curPc : -1.0f);
    }
private:
    struct Res
    {
        double b0 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;   // band-pass: b1 = 0, b2 = -b0
        void reset() { z1 = z2 = 0; }
        void set (double f, double q, double sr)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * juce::jlimit (5.0, sr * 0.45, f) / sr;
            const double al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
            b0 = al / a0; b2 = -al / a0; a1 = -2.0 * std::cos (w) / a0; a2 = (1.0 - al) / a0;
        }
        inline double process (double x)
        {
            const double y = b0 * x + z1;
            z1 = -a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void rebuildIfNeeded (const FxContext& c)
    {
        // everything that moves a resonance, folded into one key
        double key = c.mode * 1.0e7 + c.p[0] * 1.0e5 + c.p[1] * 1.0e3 + c.p[2] * 7.0 + c.p[3] * 0.013 + c.p[4] * 0.0007
                     + c.p[5] * 3.1 + c.p[6] * 0.37 + c.p[8] * 0.11 + c.dataGen * 1.0e9 + sr;
        if (key == lastKey) return;
        lastKey = key;
        float hz[harmres::kMaxRes];
        count = harmres::notes (c.p, c.mode, c.data, hz, gain, harmres::kMaxRes, pcOf);
        const double q = harmres::qOf (c.p[5]);
        const double det = std::pow (2.0, c.p[8] * 0.5 / 1200.0);
        for (int r = 0; r < count; ++r)
        {
            resL[r].set (hz[r] / det, q, sr);
            resR[r].set (hz[r] * det, q, sr);
        }
    }
    Res resL[harmres::kMaxRes], resR[harmres::kMaxRes];
    float gain[harmres::kMaxRes] {}, feed[harmres::kMaxRes] {};
    int pcOf[harmres::kMaxRes] {};
    int count = 0;
    double lastKey = -1.0e9;
    // arp
    double freeBeat = 0.0;
    long long lastStep = -1;
    int curPc = -1;
    juce::Random rng;
};

// ------------------------------------------------------------------ Talkbox
// Detail, Window, Bright, Whiten, Gate, Out | Ext
// The voice on the sidechain shapes the input (a synth) like a mouth shapes
// the sound in a talk box tube. Linear prediction, not a filter bank:
//   every quarter window the voice (pre-emphasised: Bright) is windowed and
//   solved for an all-pole vocal-tract model (Levinson-Durbin, Detail poles);
//   the carrier is flattened by its own short model (Whiten) so its harmonics
//   are even, then runs through the voice's all-pole filter. Both filters are
//   lattices whose reflection coefficients glide between analyses (always
//   stable, no clicks); the level follows the voice's prediction residual,
//   and the emphasis is undone at the end. No sidechain: the input passes.
// Taps: spec[] = the vocal-tract curve (dB) on a log axis 60 Hz .. 8 kHz.
class Talkbox : public FxImpl
{
public:
    static constexpr int kMaxOrder = 48, kCarOrder = 16, kMaxN = 4096;
    void prepare (double) override
    {
        voice.assign (kMaxN, 0.0f); car.assign (kMaxN, 0.0f); win.assign (kMaxN, 0.0f); work.assign (kMaxN, 0.0f);
        lastA.reserve (kMaxOrder + 1);   // (analyse runs on the audio thread: no allocation there)
        reset();
    }
    void reset() override
    {
        std::fill (voice.begin(), voice.end(), 0.0f); std::fill (car.begin(), car.end(), 0.0f);
        w = 0; hopCount = 0; frameN = 0; vPrev = 0.0f;
        for (auto* a : { kv, kvTo, dkv, kc, kcTo, dkc }) std::fill (a, a + kMaxOrder + 1, 0.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (fir[ch], fir[ch] + kMaxOrder + 1, 0.0f);
            std::fill (iir[ch], iir[ch] + kMaxOrder + 1, 0.0f);
            resRms[ch] = rawRms[ch] = 1.0e-6f; yPrev[ch] = 0.0f;
        }
        gainV = gainTo = dGain = 0.0f; left = 0;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const bool hasVoice = c.scL != nullptr && c.scR != nullptr;
        taps->set (1, hasVoice ? 1.0f : 0.0f);
        if (! hasVoice) return;   // nothing to speak with: the input passes
        const int order = juce::jlimit (12, kMaxOrder, (int) std::lround (c.p[0]));
        const int N = juce::jlimit (256, kMaxN, (int) (c.p[1] * 0.001f * (float) sr));
        if (N != frameN)
        {
            frameN = N; winE = 0.0f;
            for (int j = 0; j < N; ++j) { win[(size_t) j] = 0.5f - 0.5f * std::cos (kTwoPi * (float) j / (float) (N - 1)); winE += win[(size_t) j] * win[(size_t) j]; }
        }
        const int hop = juce::jmax (32, N / 4);
        const float pre = 0.4f + 0.55f * c.p[2];
        const float whiten = c.p[3], gateDb = c.p[4], outG = dbToGain (c.p[5]);
        const float rmsCoef = 1.0f / (float) N;
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (c.scL[i] + c.scR[i]);
            voice[(size_t) w] = v - pre * vPrev; vPrev = v;
            car[(size_t) w] = 0.5f * (L[i] + R[i]);
            w = (w + 1) % kMaxN;
            if (++hopCount >= hop) { hopCount = 0; analyse (order, pre, gateDb, hop); }
            if (left > 0)
            {
                --left;
                for (int m = 1; m <= order; ++m) { kv[m] += dkv[m]; }
                for (int m = 1; m <= kCarOrder; ++m) { kc[m] += dkc[m]; }
                gainV += dGain;
            }
            float* io[2] = { L + i, R + i };
            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = *io[ch];
                // flatten the carrier: lattice FIR with its own reflection coefficients
                float f = x, gcur = x;
                for (int m = 1; m <= kCarOrder; ++m)
                {
                    const float gd = fir[ch][m - 1];
                    const float fn = f + kc[m] * gd, gn = kc[m] * f + gd;
                    fir[ch][m - 1] = gcur;
                    f = fn; gcur = gn;
                }
                resRms[ch] += (f * f - resRms[ch]) * rmsCoef;
                rawRms[ch] += (x * x - rawRms[ch]) * rmsCoef;
                const float exc = whiten * f / std::sqrt (resRms[ch] + 1.0e-12f) + (1.0f - whiten) * x / std::sqrt (rawRms[ch] + 1.0e-12f);
                // the voice's vocal tract: all-pole lattice
                float y = exc;
                for (int m = order; m >= 1; --m)
                {
                    y -= kv[m] * iir[ch][m - 1];
                    iir[ch][m] = kv[m] * y + iir[ch][m - 1];
                }
                iir[ch][0] = y;
                y *= gainV;
                // undo the pre-emphasis
                y += pre * yPrev[ch];
                yPrev[ch] = y;
                *io[ch] = loopSat (y) * outG;
            }
        }
    }
private:
    // last N samples of a ring, windowed, into work[0..N)
    void frame (const std::vector<float>& ring)
    {
        for (int j = 0; j < frameN; ++j) work[(size_t) j] = ring[(size_t) ((w - frameN + j + kMaxN) % kMaxN)] * win[(size_t) j];
    }
    // autocorrelation + Levinson-Durbin: reflection coefficients k[1..p] (A(z) = 1 + sum a_j z^-j); returns the residual energy
    float levinson (int p, float* k)
    {
        double r[kMaxOrder + 1];
        for (int lag = 0; lag <= p; ++lag)
        {
            double acc = 0.0;
            for (int j = lag; j < frameN; ++j) acc += (double) work[(size_t) j] * work[(size_t) (j - lag)];
            r[lag] = acc;
        }
        r[0] = r[0] * 1.0001 + 1.0e-12;   // a touch of white noise keeps the model well-conditioned
        double a[kMaxOrder + 1] = { 1.0 }, tmp[kMaxOrder + 1];
        double e = r[0];
        for (int m = 1; m <= p; ++m)
        {
            double acc = r[m];
            for (int j = 1; j < m; ++j) acc += a[j] * r[m - j];
            double km = -acc / e;
            km = juce::jlimit (-0.999, 0.999, km);
            for (int j = 1; j < m; ++j) tmp[j] = a[j] + km * a[m - j];
            for (int j = 1; j < m; ++j) a[j] = tmp[j];
            a[m] = km;
            k[m] = (float) km;
            e *= (1.0 - km * km);
        }
        lastA.assign (a, a + p + 1);
        return (float) juce::jmax (0.0, e);
    }
    void analyse (int order, float pre, float gateDb, int hop)
    {
        frame (voice);
        double e0 = 0.0;
        for (int j = 0; j < frameN; ++j) e0 += (double) work[(size_t) j] * work[(size_t) j];
        const float resE = levinson (order, kvTo);
        for (int m = order + 1; m <= kMaxOrder; ++m) kvTo[m] = 0.0f;
        // level: the voice's residual; below the gate the mouth closes
        const float levelDb = 10.0f * std::log10 ((float) e0 / juce::jmax (1.0e-9f, winE) + 1.0e-12f);
        gainTo = levelDb < gateDb ? 0.0f : std::sqrt (resE / juce::jmax (1.0e-9f, winE));
        // the vocal-tract curve for the display
        for (int b = 0; b < VizTaps::kSpec; ++b)
        {
            const double f = 60.0 * std::pow (8000.0 / 60.0, (double) b / (double) (VizTaps::kSpec - 1));
            const double wv = kTwoPi * f / sr;
            double re = 0.0, im = 0.0;
            for (int j = 0; j < (int) lastA.size(); ++j) { re += lastA[(size_t) j] * std::cos (wv * j); im -= lastA[(size_t) j] * std::sin (wv * j); }
            const double mag = gainTo / juce::jmax (1.0e-9, std::sqrt (re * re + im * im));
            // (undo the emphasis so the curve shows the voice as heard)
            const double deemph = 1.0 / std::sqrt (1.0 + pre * pre - 2.0 * pre * std::cos (wv));
            taps->spec[(size_t) b].store ((float) (20.0 * std::log10 (mag * deemph + 1.0e-9)), std::memory_order_relaxed);
        }
        frame (car);
        levinson (kCarOrder, kcTo);
        for (int m = 1; m <= kMaxOrder; ++m) dkv[m] = (kvTo[m] - kv[m]) / (float) hop;
        for (int m = 1; m <= kCarOrder; ++m) dkc[m] = (kcTo[m] - kc[m]) / (float) hop;
        dGain = (gainTo - gainV) / (float) hop;
        left = hop;
        taps->set (0, levelDb);
    }

    std::vector<float> voice, car, win, work;
    std::vector<double> lastA { 1.0 };
    int w = 0, hopCount = 0, frameN = 0, left = 0;
    float vPrev = 0.0f, winE = 1.0f;
    float kv[kMaxOrder + 1] {}, kvTo[kMaxOrder + 1] {}, dkv[kMaxOrder + 1] {};
    float kc[kMaxOrder + 1] {}, kcTo[kMaxOrder + 1] {}, dkc[kMaxOrder + 1] {};
    float fir[2][kMaxOrder + 1] {}, iir[2][kMaxOrder + 1] {};
    float resRms[2] {}, rawRms[2] {}, yPrev[2] {};
    float gainV = 0.0f, gainTo = 0.0f, dGain = 0.0f;
};

} // namespace

std::unique_ptr<FxImpl> makeStereoPitchFx (const juce::String& n)
{
    if (n == "Talkbox")            return std::make_unique<Talkbox>();
    if (n == "Harmonic Resonator") return std::make_unique<HarmResonator>();
    if (n == "Stereo Width")      return std::make_unique<StereoWidth>();
    if (n == "Haas Effect")       return std::make_unique<Haas>();
    if (n == "Mono Bass")         return std::make_unique<MonoBass>();
    if (n == "Frequency Shifter") return std::make_unique<FreqShift>();
    if (n == "Pitch Shifter")     return std::make_unique<PitchShift>();
    if (n == "Pitch Correction")  return std::make_unique<PitchCorrection>();
    if (n == "Harmonizer")        return std::make_unique<Harmonizer>();
    if (n == "Vocoder")           return std::make_unique<Vocoder>();
    if (n == "Resonator")         return std::make_unique<Resonator>();
    return nullptr;
}

} // namespace efx
