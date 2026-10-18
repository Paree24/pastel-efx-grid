#include "FxCommon.h"

// Spectral (zero-latency filter banks) + Time (buffer effects).
// Meter conventions (VizTaps::m):
//   Spectral Comp / Resonance Supp.: m[0..15] band gain dB, m[39] band count
//   Exciter: m[0] added harmonics level dB
//   Granular: m[0..31] grain ages (0..1, <0 idle), m[33..] unused
//   Time Stretch / Reverse / Stutter / Shuffler / Slicer / Glitch:
//     m[0] cycle / step position 0..1, m[1] state (event type, step index...), m[2] gain
//   Tape Stop: m[0] current speed

namespace efx
{
namespace
{

// ------------------------------------------------------------------ Spectral Comp
// Thresh, Ratio, Attack, Release, Bands, Knee, Makeup, Range, Delta | Down / Up
// Shape data: per-band threshold offsets (dB), up to 16 bands.
class SpectralComp : public FxImpl
{
public:
    static constexpr int kMax = 16;
    static constexpr int kCounts[3] = { 4, 8, 16 };
    bool usesData() const override { return true; }
    void defaultData (float* d) const override { for (int b = 0; b < kMax; ++b) d[b] = 0.0f; }
    void prepare (double) override { configure (16); reset(); }
    void configure (int nb)
    {
        bands = nb;
        spL.prepare (sr, nb, 120.0f, 9000.0f); spR.prepare (sr, nb, 120.0f, 9000.0f);
    }
    void reset() override { spL.reset(); spR.reset(); for (auto& e : env) e = 0.0f; for (auto& g : gs) g.reset(); }
    // gain change (dB) for band level x
    static float bandGain (float x, float th, float ratio, float knee, float range, bool up)
    {
        if (! up) return juce::jmax (-range, compCurve (x, th, ratio, knee));
        if (x <= -70.0f) return 0.0f;
        // upward: mirror of the downward curve below the threshold
        return juce::jmin (range, -compCurve (2.0f * th - x, th, ratio, knee));
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int nb = kCounts[juce::jlimit (0, 2, ci (c, 4))];
        if (nb != bands) { configure (nb); reset(); }
        const float th = c.p[0], ratio = c.p[1], knee = c.p[5], range = c.p[7], makeup = dbToGain (c.p[6]);
        const float atk = msCoef (c.p[2], sr), rel = msCoef (c.p[3], sr);
        const float eA = msCoef (1.0f, sr), eR = msCoef (30.0f, sr);
        const bool up = c.mode == 1, delta = on (c, 8);
        float bth[kMax];
        for (int b = 0; b < nb; ++b) bth[b] = th + juce::jlimit (-24.0f, 24.0f, c.data[b]);
        float last[kMax] {}, lvl[kMax] {};
        for (int i = 0; i < n; ++i)
        {
            float bl[kMax], br[kMax];
            spL.split (L[i], bl); spR.split (R[i], br);
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < nb; ++b)
            {
                const float a = juce::jmax (std::abs (bl[b]), std::abs (br[b]));
                env[b] += (a > env[b] ? eA : eR) * (a - env[b]);
                const float x = gainToDb (env[b]);
                const float target = bandGain (x, bth[b], ratio, knee, range, up);
                const float g = up ? gs[b].process (target, rel, atk) : gs[b].process (target, atk, rel);
                const float lin = dbToGain (g);
                ol += bl[b] * lin; orr += br[b] * lin;
                last[b] = g; lvl[b] = x;
            }
            ol *= makeup; orr *= makeup;
            if (delta) { ol = L[i] - ol; orr = R[i] - orr; }
            // safety ceiling: upward gain + makeup can reach +40 dB on a hit
            // that follows silence (the gain hasn't come down yet)
            L[i] = safeCeiling (ol); R[i] = safeCeiling (orr);
        }
        for (int b = 0; b < kMax; ++b) { taps->set (b, b < nb ? last[b] : 0.0f); taps->set (16 + b, b < nb ? lvl[b] : -120.0f); }
        taps->set (VizTaps::kMeters - 1, (float) nb);
    }
private:
    BandSplitter<kMax> spL, spR;
    int bands = 16;
    float env[kMax] {};
    GainSmoother gs[kMax];
};

// ------------------------------------------------------------------ Resonance suppressor
// Depth, Sens, Low, High, Attack, Release, Delta, Bands, Sharpness, Tilt | Soft / Hard
class ResonanceSupp : public FxImpl
{
public:
    static constexpr int kMax = 32;
    static constexpr int kCounts[3] = { 16, 24, 32 };
    void prepare (double) override { configure (24); reset(); }
    void configure (int nb) { bands = nb; spL.prepare (sr, nb, 60.0f, 14000.0f); spR.prepare (sr, nb, 60.0f, 14000.0f); }
    void reset() override { spL.reset(); spR.reset(); for (auto& e : env) e = 0.0f; for (auto& g : gs) g.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int nb = kCounts[juce::jlimit (0, 2, ci (c, 7))];
        if (nb != bands) { configure (nb); reset(); }
        const float depth = c.p[0], thr = 9.0f - c.p[1] * 12.0f;
        const float lo = c.p[2], hi = juce::jmax (c.p[2], c.p[3]);
        const float atk = msCoef (c.p[4], sr), rel = msCoef (c.p[5], sr);
        const bool delta = on (c, 6), hard = c.mode == 1;
        // sharpness: how far away the "neighbours" are (wide = broad bumps count as resonances too)
        const int nearEx = 1, reach = 2 + (int) std::lround ((1.0f - c.p[8]) * 4.0f * (float) nb / 24.0f);
        const float tilt = c.p[9];
        const float eA = msCoef (1.0f, sr), eR = msCoef (25.0f, sr);
        bool active[kMax];
        float bandDepth[kMax];
        for (int b = 0; b < nb; ++b)
        {
            const float f = spL.centre (b);
            active[b] = f >= lo && f <= hi;
            bandDepth[b] = depth * juce::jlimit (0.0f, 2.0f, 1.0f + tilt * (2.0f * (float) b / (float) (nb - 1) - 1.0f));
        }
        float last[kMax] {};
        for (int i = 0; i < n; ++i)
        {
            float bl[kMax], br[kMax], lv[kMax];
            spL.split (L[i], bl); spR.split (R[i], br);
            for (int b = 0; b < nb; ++b)
            {
                const float a = juce::jmax (std::abs (bl[b]), std::abs (br[b]));
                env[b] += (a > env[b] ? eA : eR) * (a - env[b]);
                lv[b] = gainToDb (env[b]);
            }
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < nb; ++b)
            {
                float sum = 0.0f; int cnt = 0;
                for (int k = juce::jmax (0, b - reach); k <= juce::jmin (nb - 1, b + reach); ++k)
                    if (std::abs (k - b) > nearEx) { sum += lv[k]; ++cnt; }
                const float excess = lv[b] - sum / (float) juce::jmax (1, cnt) - thr;
                const float target = (active[b] && excess > 0.0f && lv[b] > -70.0f) ? -juce::jmin (bandDepth[b], excess * (hard ? 1.0f : 0.5f)) : 0.0f;
                const float g = gs[b].process (target, atk, rel);
                const float lin = dbToGain (g);
                ol += bl[b] * lin; orr += br[b] * lin;
                last[b] = g;
            }
            if (delta) { L[i] -= ol; R[i] -= orr; }
            else       { L[i] = ol; R[i] = orr; }
        }
        for (int b = 0; b < kMax; ++b) taps->set (b, b < nb ? last[b] : 0.0f);
        taps->set (VizTaps::kMeters - 1, (float) nb);
    }
private:
    BandSplitter<kMax> spL, spR;
    int bands = 24;
    float env[kMax] {};
    GainSmoother gs[kMax];
};

// ------------------------------------------------------------------ Harmonic exciter
// Amount, Freq, Character, Drive, Out, Tone, Dynamic, Width | High / Low
// High: harmonics generated from the content above Freq. Low: from the bass
// below Freq / 20 (adds audible upper harmonics to sub content).
class Exciter : public FxImpl
{
public:
    void prepare (double s) override { os.prepare (GridEffect::kChunk, 1); osSr = s * 2.0; reset(); }
    void reset() override
    {
        os.reset(); for (auto& b : split) b.reset(); for (auto& b : post) b.reset();
        lastF = -1.0f; lastMode = -1; lvl = -100.0f; fast.reset(); slow.reset(); apL.reset(); apR.reset(); tl.reset(); tr.reset();
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const bool low = c.mode == 1;
        const float fx = low ? c.p[1] / 20.0f : c.p[1];
        if (std::abs (fx - lastF) > 0.5f || lastMode != c.mode)
        {
            lastF = fx; lastMode = c.mode;
            for (auto& b : split) { if (low) b.setLowPass (fx, 0.707f, osSr); else b.setHighPass (fx, 0.707f, osSr); }
            for (auto& b : post) b.setHighPass (low ? fx * 1.5f : fx, 0.707f, osSr);
        }
        const float amt = c.p[0] * 2.0f, chr = (c.p[2] + 1.0f) * 0.5f, g = 1.0f + c.p[3] * 8.0f;
        const float out = dbToGain (c.p[4]), dyn = c.p[6], width = c.p[7];
        tl.setCutoff (c.p[5], osSr); tr.setCutoff (c.p[5], osSr);
        fast.set (0.5f, 30.0f, osSr); slow.set (20.0f, 250.0f, osSr);
        const bool tone = c.p[5] < 19000.0f;
        float pk = 0.0f;
        os.process (L, R, n, [&] (float& l, float& r)
        {
            const float lvlF = fast.process (0.5f * (l + r)), lvlS = slow.process (0.5f * (l + r));
            // dynamic: favour transients (fast above slow) over steady parts
            const float dynG = lerp (1.0f, juce::jlimit (0.0f, 3.0f, lvlF / juce::jmax (1.0e-5f, lvlS)), dyn);
            float add[2];
            float* ch[2] = { &l, &r };
            for (int k = 0; k < 2; ++k)
            {
                const float h = split[k].process (*ch[k]);
                const float odd = fastTanh (h * g) / g - h * 0.9f;        // mostly 3rd, 5th...
                const float even = h * std::abs (h) * g;                  // 2nd, 4th...
                float a = post[k].process (lerp (odd, even, chr)) * amt * dynG;
                if (tone) a = (k == 0 ? tl : tr).lp (a);
                add[k] = a;
                pk = juce::jmax (pk, std::abs (a));
            }
            // width: decorrelate the added harmonics between the sides
            if (width > 0.001f)
            {
                const float wl = apL.process (add[0], 0.6f), wr = apR.process (add[1], -0.6f);
                add[0] = lerp (add[0], wl, width); add[1] = lerp (add[1], wr, width);
            }
            l = (l + add[0]) * out; r = (r + add[1]) * out;
        });
        lvl = juce::jmax (gainToDb (pk), lvl - 1.0f);
        taps->set (0, lvl);
    }
private:
    Oversampler os;
    double osSr = 88200.0;
    Biquad split[2], post[2];
    float lastF = -1.0f, lvl = -100.0f;
    int lastMode = -1;
    EnvFollower fast, slow;
    Allpass1 apL, apR;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Granular
// Grain, Density, Spray, Pitch, Detune, Reverse, Width, Freeze, Position, Window, Quantize, Tone
class Granular : public FxImpl
{
public:
    static constexpr int kG = 32;
    struct Grain { double pos = 0.0, inc = 1.0; int len = 0, age = 0; float gl = 1.0f, gr = 1.0f; bool active = false; };
    void prepare (double s) override { hist.prepare (s, 3.0f); rng.seed (21); reset(); }
    void reset() override { hist.reset(); for (auto& g : grains) g.active = false; spawn = 0.0; frozenAt = -1; tl.reset(); tr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int window = ci (c, 9), quant = ci (c, 10);
        const double posS = c.p[8] * 0.001 * sr;
        tl.setCutoff (c.p[11], sr); tr.setCutoff (c.p[11], sr);
        const bool tone = c.p[11] < 19000.0f;
        const int glen = juce::jmax (16, (int) (c.p[0] * 0.001f * sr));
        const float rate = 3.0f + c.p[1] * 77.0f;
        const float spray = c.p[2], rev = c.p[5], width = c.p[6];
        const bool freeze = on (c, 7);
        for (int i = 0; i < n; ++i)
        {
            if (! freeze) { hist.write (L[i], R[i]); frozenAt = -1; }
            else if (frozenAt < 0) frozenAt = hist.w;
            const long long now = freeze ? frozenAt : hist.w;
            spawn += rate / sr;
            if (spawn >= 1.0)
            {
                spawn -= 1.0;
                for (auto& g : grains)
                    if (! g.active)
                    {
                        const double ratio = semisToRatio (c.p[3] + quantizedDetune (rng, c.p[4] * 12.0f, quant));
                        const bool back = rng.uni() < rev;
                        const double reach = (double) glen * juce::jmax (1.0, ratio) + 4.0;
                        const double start = (double) now - reach - posS - spray * rng.uni() * 1.4 * sr;
                        g.inc = back ? -ratio : ratio;
                        g.pos = back ? start + reach : start;
                        g.len = glen; g.age = 0; g.active = true;
                        panGains (rng.bipolar() * width, g.gl, g.gr);
                        break;
                    }
            }
            float ol = 0.0f, orr = 0.0f;
            for (auto& g : grains)
            {
                if (! g.active) continue;
                const float w = grainWindow (window, (float) g.age / (float) g.len);
                float a, b;
                hist.read (g.pos, a, b);
                ol += a * w * g.gl; orr += b * w * g.gr;
                g.pos += g.inc;
                if (++g.age >= g.len) g.active = false;
            }
            const float norm = 1.0f / std::sqrt (juce::jmax (1.0f, rate * (float) glen / (float) sr));
            L[i] = ol * norm; R[i] = orr * norm;
            if (tone) { L[i] = tl.lp (L[i]); R[i] = tr.lp (R[i]); }
        }
        int k = 0;
        for (auto& g : grains) if (k < VizTaps::kMeters) taps->set (k++, g.active ? (float) g.age / (float) juce::jmax (1, g.len) : -1.0f);
    }
private:
    History hist;
    Grain grains[kG];
    Rng rng;
    double spawn = 0.0;
    long long frozenAt = -1;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Time stretch (loop half-time)
// Ratio, Length, Grain, Fade, Pitch, Tone | Keep / Tape
class TimeStretch : public FxImpl
{
public:
    void prepare (double s) override { hist.prepare (s, 17.0f); reset(); }
    void reset() override { hist.reset(); cycle = -1; cycleStart = 0; tl.reset(); tr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double pitch = semisToRatio (c.p[4]);
        tl.setCutoff (c.p[5], sr); tr.setCutoff (c.p[5], sr);
        const bool tone = c.p[5] < 19000.0f;
        static constexpr double kRatio[3] = { 0.5, 1.0 / 3.0, 0.25 };
        static constexpr double kLen[4] = { 1.0, 2.0, 4.0, 8.0 };
        const double ratio = kRatio[juce::jlimit (0, 2, ci (c, 0))];
        const double lenB = kLen[juce::jlimit (0, 3, ci (c, 1))];
        const int grain = juce::jmax (64, (int) (c.p[2] * 0.001f * sr));
        const float fadeN = juce::jmax (1.0f, c.p[3] * 0.001f * (float) sr);
        const bool tape = c.mode == 1;
        double frac = 0.0;
        for (int i = 0; i < n; ++i)
        {
            hist.write (L[i], R[i]);
            const double beat = c.ppq + c.beatsPerSample * i;
            const long long cyc = (long long) std::floor (beat / lenB);
            if (cyc != cycle) { cycle = cyc; cycleStart = hist.w - 1; }
            const double t = (double) (hist.w - 1 - cycleStart);        // samples since the cycle began
            const double cycleLen = c.samplesFor (lenB);
            frac = t / juce::jmax (1.0, cycleLen);
            const double src = (double) cycleStart + t * ratio;
            float a, b;
            if (tape) hist.read (src, a, b);
            else
            {
                // two grains, half a grain apart, each reading at normal speed
                // (times Pitch) around the slowed-down source position
                const double ph = std::fmod (t, (double) grain) / grain;
                float a1, b1, a2, b2;
                const double off1 = ph * grain * pitch, off2 = std::fmod (ph + 0.5, 1.0) * grain * pitch;
                hist.read (juce::jmin ((double) hist.w - 2.0, src - grain + off1), a1, b1);
                hist.read (juce::jmin ((double) hist.w - 2.0, src - grain + off2), a2, b2);
                const float w1 = std::sin (kPi * (float) ph), w2 = std::sin (kPi * (float) std::fmod (ph + 0.5, 1.0));
                a = a1 * w1 + a2 * w2; b = b1 * w1 + b2 * w2;
            }
            const float edge = juce::jmin (1.0f, (float) t / fadeN, (float) (cycleLen - t) / fadeN);
            const float g = clamp01 (edge);
            if (tone) { a = tl.lp (a); b = tr.lp (b); }
            L[i] = a * g + L[i] * (1.0f - g);
            R[i] = b * g + R[i] * (1.0f - g);
        }
        taps->set (0, (float) frac); taps->set (1, (float) ratio);
    }
private:
    History hist;
    long long cycle = -1, cycleStart = 0;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Reverse
// Sync, Time, Div, Fade, Feedback, Speed, Tone, Width | Reverse / Alternate
class Reverse : public FxImpl
{
public:
    void prepare (double s) override { hist.prepare (s, 9.0f); reset(); }
    void reset() override { hist.reset(); seg = -1; segStart = 0; prevStart = 0; segLen = 1; free = 0.0; fbl = fbr = 0.0f; tl.reset(); tr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double speed = c.p[5];
        const double widthS = c.p[7] * 0.02 * sr;   // the right side reads a little later
        tl.setCutoff (c.p[6], sr); tr.setCutoff (c.p[6], sr);
        const bool tone = c.p[6] < 19000.0f;
        const bool sync = on (c, 0);
        const double beats = divBeatsOf (*def, c.p, 2);
        const double lenS = sync ? c.samplesFor (beats) : c.p[1] * 0.001 * sr;
        const float fade = c.p[3], fb = c.p[4];
        const bool alt = c.mode == 1;
        float pos01 = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            hist.write (L[i] + loopSat (fbl * fb), R[i] + loopSat (fbr * fb));
            long long s;
            if (sync) { const double beat = c.ppq + c.beatsPerSample * i; s = (long long) std::floor (beat / beats); }
            else { free += 1.0; s = (long long) std::floor (free / juce::jmax (64.0, lenS)); }
            if (s != seg)
            {
                seg = s; prevStart = segStart; segStart = hist.w - 1;
                segLen = std::max<long long> (64, segStart - prevStart);
            }
            const double t = (double) (hist.w - 1 - segStart) * speed;
            const double u = t / (double) segLen;
            pos01 = (float) u;
            const bool backwards = ! alt || (seg & 1) == 0;
            // play the previous segment (fully recorded) backwards / forwards, at Speed
            const double src = backwards ? (double) segStart - 1.0 - t : (double) prevStart + t;
            float a = 0.0f, b = 0.0f, a2 = 0.0f, b2 = 0.0f;
            if (u < 1.0) { hist.read (src, a, b); if (widthS > 1.0) hist.read (src + (backwards ? widthS : -widthS), a2, b2); else b2 = b; }
            const float w = juce::jmin (1.0f, (float) u / juce::jmax (0.005f, fade), (float) (1.0 - u) / juce::jmax (0.005f, fade));
            fbl = a * clamp01 (w); fbr = b2 * clamp01 (w);
            if (tone) { fbl = tl.lp (fbl); fbr = tr.lp (fbr); }
            L[i] = fbl; R[i] = fbr;
        }
        taps->set (0, pos01); taps->set (1, (float) (seg & 1));
    }
private:
    History hist;
    long long seg = -1, segStart = 0, prevStart = 0, segLen = 1;
    double free = 0.0;
    float fbl = 0.0f, fbr = 0.0f;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Tape stop
// Stop, Speed, Stop Time, Start Time, Curve, Div, Darken, Wow | Manual / Loop
class TapeStop : public FxImpl
{
public:
    void prepare (double s) override { hist.prepare (s, 12.0f); reset(); }
    void reset() override { hist.reset(); v = 1.0f; readPos = 0.0; xfade = 0.0f; xfFrom = 0.0; tl.reset(); tr.reset(); wow.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        bool stop = on (c, 0);
        if (c.mode == 1)
        {
            const double beats = divBeatsOf (*def, c.p, 5);
            const double ph = c.ppq / beats - std::floor (c.ppq / beats);
            stop = stop || ph >= 0.5;
        }
        const float target = stop ? 0.0f : c.p[1];
        const float down = 1.0f / (juce::jmax (1.0f, c.p[2] * 0.001f * (float) sr));
        const float up = 1.0f / (juce::jmax (1.0f, c.p[3] * 0.001f * (float) sr));
        const float expo = std::pow (2.0f, c.p[4] * 2.0f);
        const double maxLag = (double) hist.size - sr * 1.0;
        float rate = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            hist.write (L[i], R[i]);
            if (v > target) v = juce::jmax (target, v - down); else v = juce::jmin (target, v + up);
            rate = std::pow (clamp01 (v), expo);
            if (c.p[7] > 0.001f && rate > 0.0f && rate < 0.999f) { rate *= 1.0f + c.p[7] * 0.04f * wow.valueAt (0, 0.0); wow.advance (0.8 / sr); }
            if (readPos <= 0.0) readPos = (double) hist.w - 1.0;
            readPos += rate;
            const double live = (double) hist.w - 1.0;
            if (readPos > live) readPos = live;
            // back at normal speed (or lagging too far): crossfade to the live signal
            const bool catchUp = (rate > 0.999f && live - readPos > 1.0) || live - readPos > maxLag;
            if (catchUp && xfade <= 0.0f) { xfade = 1.0f; xfFrom = readPos; readPos = live; }
            float a, b;
            hist.read (readPos, a, b);
            if (xfade > 0.0f)
            {
                float oa, ob;
                xfFrom += rate;
                hist.read (xfFrom, oa, ob);
                a = a * (1.0f - xfade) + oa * xfade; b = b * (1.0f - xfade) + ob * xfade;
                xfade -= 1.0f / (0.02f * (float) sr);
            }
            // a slowing tape loses its top end
            if (c.p[6] > 0.001f && rate < 0.999f)
            {
                const float fc = 300.0f + 19700.0f * std::pow (clamp01 (rate), 1.0f + 2.0f * c.p[6]);
                tl.setCutoff (fc, sr); tr.setCutoff (fc, sr);
                a = tl.lp (a); b = tr.lp (b);
            }
            else { tl.lp (a); tr.lp (b); }
            L[i] = a; R[i] = b;
        }
        taps->set (0, rate);
    }
private:
    History hist;
    float v = 1.0f, xfade = 0.0f;
    double readPos = 0.0, xfFrom = 0.0;
    OnePole tl, tr;
    Lfo wow;
};

// ------------------------------------------------------------------ Stutter
// Div, Chance, Repeats, Gate, Pitch, Fade, Hold, Decay, Reverse, Filter
class Stutter : public FxImpl
{
public:
    void prepare (double s) override { hist.prepare (s, 8.0f); rng.seed (41); reset(); }
    void reset() override { hist.reset(); step = -1; active = false; rep = 0; evStart = 0; stepStart = 0; fl.reset(); fr.reset(); reversed = false; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double beats = divBeatsOf (*def, c.p, 0);
        const int repeats = juce::jmax (1, ci (c, 2));
        const float decayDb = c.p[7], filt = c.p[9];
        const float gate = c.p[3], pitch = c.p[4];
        const float fadeN = juce::jmax (1.0f, c.p[5] * 0.001f * (float) sr);
        const bool hold = on (c, 6);
        const double stepLen = c.samplesFor (beats);
        float ramp = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            hist.write (L[i], R[i]);
            const double beat = c.ppq + c.beatsPerSample * i;
            const long long s = (long long) std::floor (beat / beats);
            if (s != step)
            {
                step = s; stepStart = hist.w - 1;
                if (active) ++rep;
                if (active && rep >= repeats && ! hold) active = false;
                if (! active && (hold || rng.uni() < c.p[1])) { active = true; rep = 0; evStart = stepStart; reversed = rng.uni() < c.p[8]; }
                // each repeat: quieter, and filtered further (low-pass when Filter < 0, high-pass when > 0)
                const float sweep = clamp01 (std::abs (filt) * (float) rep / (float) juce::jmax (1, repeats - 1));
                const float fc = filt < 0.0f ? 20000.0f * std::pow (0.02f, sweep) : 20.0f * std::pow (400.0f, sweep);
                fl.setCutoff (fc, sr); fr.setCutoff (fc, sr);
            }
            const double t = (double) (hist.w - 1 - stepStart);
            const double u = t / juce::jmax (1.0, stepLen);
            ramp = (float) u;
            float a = L[i], b = R[i];
            if (active)
            {
                if (rep > 0)
                {
                    const double rate = semisToRatio (pitch * (float) rep);
                    const double tt = t * rate;
                    hist.read (reversed ? (double) evStart + stepLen - tt : (double) evStart + tt, a, b);
                    const float lvl = dbToGain (decayDb * (float) rep);
                    a *= lvl; b *= lvl;
                    if (std::abs (filt) > 0.01f)
                    {
                        if (filt < 0.0f) { a = fl.lp (a); b = fr.lp (b); }
                        else { a = fl.hp (a); b = fr.hp (b); }
                    }
                }
                const float g = u < gate ? clamp01 (juce::jmin ((float) t / fadeN, (float) (gate * stepLen - t) / fadeN)) : 0.0f;
                const float edge = rep > 0 ? g : (u < gate ? 1.0f : g);
                a *= edge; b *= edge;
            }
            L[i] = a; R[i] = b;
        }
        taps->set (0, ramp); taps->set (1, active ? (float) (rep + 1) : 0.0f);
    }
private:
    History hist;
    Rng rng;
    long long step = -1, evStart = 0, stepStart = 0;
    int rep = 0;
    bool active = false, reversed = false;
    OnePole fl, fr;
};

// ------------------------------------------------------------------ Buffer shuffler
// Div, Chance, Range, Reverse, Repeat, Smooth, Pitch, Gate
class Shuffler : public FxImpl
{
public:
    void prepare (double s) override { hist.prepare (s, 17.0f); rng.seed (51); reset(); }
    void reset() override { hist.reset(); step = -1; src = prevSrc = 0.0; back = prevBack = false; live = prevLive = true; stepStart = 0; kind = 0; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double beats = divBeatsOf (*def, c.p, 0);
        static constexpr double kRange[3] = { 2.0, 4.0, 8.0 };
        const double rangeB = kRange[juce::jlimit (0, 2, ci (c, 2))];
        const double stepLen = c.samplesFor (beats);
        const float xfN = juce::jmax (1.0f, c.p[5] * 0.001f * (float) sr);
        for (int i = 0; i < n; ++i)
        {
            hist.write (L[i], R[i]);
            const double beat = c.ppq + c.beatsPerSample * i;
            const long long s = (long long) std::floor (beat / beats);
            if (s != step)
            {
                step = s;
                prevSrc = src + stepLen; prevBack = back; prevLive = live;
                stepStart = hist.w - 1;
                if (rng.uni() < c.p[1])
                {
                    live = false;
                    if (rng.uni() < c.p[4] && kind != 0) { /* repeat the previous slice */ }
                    else
                    {
                        const int maxBack = juce::jmax (1, (int) (rangeB / beats));
                        const int k = 1 + (int) (rng.uni() * (float) maxBack);
                        src = (double) stepStart - k * stepLen;
                    }
                    back = rng.uni() < c.p[3];
                    rate = rng.uni() < c.p[6] ? (rng.uni() < 0.5f ? 2.0 : 0.5) : 1.0;
                    kind = back ? 2 : 1;
                }
                else { live = true; kind = 0; rate = 1.0; }
            }
            const double t = (double) (hist.w - 1 - stepStart);
            auto readFrom = [&] (bool isLive, double base, bool rev, double tt, float& a, float& b)
            {
                if (isLive) { hist.read ((double) hist.w - 1.0 - (t - tt), a, b); return; }
                const double tr2 = std::fmod (tt * rate, stepLen);
                hist.read (rev ? base + stepLen - tr2 : base + tr2, a, b);
            };
            float a, b;
            readFrom (live, src, back, t, a, b);
            if (! live && c.p[7] < 0.999f)
            {
                const float edge = (float) (c.p[7] * stepLen - t) / xfN;
                const float gg = clamp01 (edge);
                a *= gg; b *= gg;
            }
            if (t < xfN)
            {
                float pa, pb;
                readFrom (prevLive, prevSrc - stepLen, prevBack, t + stepLen, pa, pb);
                const float w = (float) t / xfN;
                a = pa * (1.0f - w) + a * w; b = pb * (1.0f - w) + b * w;
            }
            L[i] = a; R[i] = b;
        }
        taps->set (0, (float) (c.ppq / beats - std::floor (c.ppq / beats))); taps->set (1, (float) kind);
    }
private:
    History hist;
    Rng rng;
    long long step = -1, stepStart = 0;
    double src = 0.0, prevSrc = 0.0;
    bool back = false, prevBack = false, live = true, prevLive = true;
    int kind = 0;
    double rate = 1.0;
};

// ------------------------------------------------------------------ Slicer (euclidean)
// Div, Steps, Pulses, Rotate, Swing, Attack, Decay, Depth, Accent, Pan
class Slicer : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { lastStep = -1; env = 0.0f; t = 0.0f; hitNow = false; level = 1.0f; hitCount = 0; rng.seed (71); }
    static bool hit (int step, int steps, int pulses, int rotate)
    {
        pulses = juce::jlimit (0, steps, pulses);
        const int s = ((step + rotate) % steps + steps) % steps;
        return ((s * pulses) % steps) < pulses;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double beats = divBeatsOf (*def, c.p, 0);
        const int steps = juce::jmax (2, ci (c, 1)), pulses = ci (c, 2), rotate = ci (c, 3);
        const float swing = c.p[4];
        const float atk = juce::jmax (1.0f, c.p[5] * 0.001f * (float) sr), dec = juce::jmax (1.0f, c.p[6] * 0.001f * (float) sr);
        const float floorG = 1.0f - c.p[7];
        int shown = 0;
        for (int i = 0; i < n; ++i)
        {
            const double beat = c.ppq + c.beatsPerSample * i;
            const double pos = beat / beats;
            long long sa = (long long) std::floor (pos);
            if ((sa & 1) == 1 && pos - (double) sa < swing) --sa;
            if (sa != lastStep)
            {
                lastStep = sa;
                hitNow = hit ((int) (((sa % steps) + steps) % steps), steps, pulses, rotate);
                if (hitNow) { t = 0.0f; level = 1.0f - c.p[8] * rng.uni(); ++hitCount; }
            }
            float e = 0.0f;
            if (hitNow) { e = t < atk ? t / atk : std::exp (-(t - atk) / dec * 2.3f); t += 1.0f; }
            env += (e * level - env) * 0.25f;
            const float g = floorG + (1.0f - floorG) * env;
            // pan: alternate hits lean left / right
            const float pan = c.p[9] * ((hitCount & 1) ? 1.0f : -1.0f);
            L[i] *= g * (1.0f - juce::jmax (0.0f, pan)); R[i] *= g * (1.0f + juce::jmin (0.0f, pan));
            shown = (int) (((sa % steps) + steps) % steps);
        }
        taps->set (0, (float) shown); taps->set (2, env);
    }
private:
    long long lastStep = -1;
    float env = 0.0f, t = 0.0f, level = 1.0f;
    int hitCount = 0;
    bool hitNow = false;
    Rng rng;
};

// ------------------------------------------------------------------ Glitch
// Density, Div, Repeat, Reverse, Crush, Pitch, Stop, Length, Filter, Pan | Subtle / Wild
class Glitch : public FxImpl
{
public:
    enum Kind { None = 0, Repeat, Reverse, Crush, Pitch, Stop };
    void prepare (double s) override { hist.prepare (s, 8.0f); rng.seed (61); reset(); }
    void reset() override { hist.reset(); step = -1; kind = None; left = 0; evStart = 0; stepStart = 0; hold = 0.0f; holdN = 0; ramp = 1.0f; stopPos = 0.0; fl.reset(); fr.reset(); pan = 0.0f; filtered = false; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double beats = divBeatsOf (*def, c.p, 1);
        const double stepLen = c.samplesFor (beats);
        const bool wild = c.mode == 1;
        const float fadeN = 0.003f * (float) sr;
        for (int i = 0; i < n; ++i)
        {
            hist.write (L[i], R[i]);
            const double beat = c.ppq + c.beatsPerSample * i;
            const long long s = (long long) std::floor (beat / beats);
            if (s != step)
            {
                step = s; stepStart = hist.w - 1;
                if (left > 0) --left;
                if (left <= 0)
                {
                    kind = None;
                    if (rng.uni() < c.p[0])
                    {
                        const float w[5] = { c.p[2], c.p[3], c.p[4], c.p[5], c.p[6] };
                        float total = 0.0f; for (float x : w) total += x;
                        if (total > 0.001f)
                        {
                            float r = rng.uni() * total; int k = 0;
                            while (k < 4 && r > w[k]) { r -= w[k]; ++k; }
                            kind = k + 1;
                            left = 1 + (int) (rng.uni() * (float) ci (c, 7) * (wild ? 2.0f : 1.0f));
                            filtered = rng.uni() < c.p[8];
                            if (filtered) { const float fc = 400.0f + 3000.0f * rng.uni(); fl.setCutoff (fc, sr); fr.setCutoff (fc, sr); }
                            pan = c.p[9] * rng.bipolar();
                            evStart = stepStart - (long long) stepLen;   // the slice just played
                            stopPos = (double) stepStart;
                            pitchUp = rng.uni() < 0.5f;
                        }
                    }
                }
            }
            const double t = (double) (hist.w - 1 - stepStart);
            const double u = std::fmod (t, stepLen);
            float a = L[i], b = R[i];
            switch (kind)
            {
                case Repeat:  hist.read ((double) evStart + u, a, b); break;
                case Reverse: hist.read ((double) evStart + stepLen - u, a, b); break;
                case Pitch:   hist.read ((double) evStart + std::fmod (u * (pitchUp ? 2.0 : 0.5), stepLen), a, b); break;
                case Crush:
                {
                    if (++holdN >= (wild ? 12 : 5)) { holdN = 0; hold = a; holdR = b; }
                    const float lv = wild ? 8.0f : 32.0f;
                    a = std::round (hold * lv) / lv; b = std::round (holdR * lv) / lv;
                    break;
                }
                case Stop:
                {
                    const double total = stepLen * (double) juce::jmax (1, left);
                    const double prog = juce::jmin (1.0, (double) (hist.w - 1 - (long long) stopPos) / juce::jmax (1.0, total));
                    stopPos += 0.0;
                    const double rate = 1.0 - prog;
                    stopRead = (stopRead <= 0.0 ? stopPos : stopRead) + rate;
                    hist.read (stopRead, a, b);
                    break;
                }
                default: stopRead = 0.0; break;
            }
            // short fades at the step edges for the buffer-reading events
            if (kind == Repeat || kind == Reverse || kind == Pitch)
            {
                const float e = clamp01 (juce::jmin ((float) u / fadeN, (float) (stepLen - u) / fadeN));
                a *= e; b *= e;
            }
            if (kind != None)
            {
                if (filtered) { a = fl.lp (a); b = fr.lp (b); }
                a *= 1.0f - juce::jmax (0.0f, pan); b *= 1.0f + juce::jmin (0.0f, pan);
            }
            L[i] = a; R[i] = b;
        }
        taps->set (0, (float) (c.ppq / beats - std::floor (c.ppq / beats))); taps->set (1, (float) kind);
    }
private:
    History hist;
    Rng rng;
    long long step = -1, evStart = 0, stepStart = 0;
    int kind = None, left = 0, holdN = 0;
    float hold = 0.0f, holdR = 0.0f, ramp = 1.0f;
    double stopPos = 0.0, stopRead = 0.0;
    bool pitchUp = true, filtered = false;
    float pan = 0.0f;
    OnePole fl, fr;
};

} // namespace

std::unique_ptr<FxImpl> makeSpectralTimeFx (const juce::String& n)
{
    if (n == "Spectral Comp")    return std::make_unique<SpectralComp>();
    if (n == "Resonance Supp.")  return std::make_unique<ResonanceSupp>();
    if (n == "Harmonic Exciter") return std::make_unique<Exciter>();
    if (n == "Granular")         return std::make_unique<Granular>();
    if (n == "Time Stretch")     return std::make_unique<TimeStretch>();
    if (n == "Reverse")          return std::make_unique<Reverse>();
    if (n == "Tape Stop")        return std::make_unique<TapeStop>();
    if (n == "Stutter")          return std::make_unique<Stutter>();
    if (n == "Buffer Shuffler")  return std::make_unique<Shuffler>();
    if (n == "Slicer")           return std::make_unique<Slicer>();
    if (n == "Glitch")           return std::make_unique<Glitch>();
    return nullptr;
}

} // namespace efx
