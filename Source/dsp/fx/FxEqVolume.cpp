#include "FxCommon.h"

// EQ + Volume. Meter conventions (VizTaps::m):
//   De-Esser m[0] gain change dB
//   Click Remover m[0] clicks per second, m[1] last repair (0..1)
//   ADSR m[0] envelope 0..1, m[1] stage
//   Transient Shaper m[0] gain change dB
//   Ducker / Trance Gate m[0] gain 0..1, m[1] position / step
// responseDb(): EQ magnitude for the curve display.

namespace efx
{
namespace
{

// one EQ band = up to two cascaded biquads (cuts are 24 dB/oct)
struct EqBand
{
    Biquad f[2];
    int count = 0;
    // type: 0 peak, 1 low shelf, 2 high shelf, 3 low cut, 4 high cut, 5 notch, 6 band pass
    void design (int type, float freq, float gain, float q, double sr)
    {
        freq = juce::jlimit (10.0f, (float) sr * 0.49f, freq);
        count = 1;
        switch (type)
        {
            case 1: f[0].setLowShelf (freq, q, gain, sr); break;
            case 2: f[0].setHighShelf (freq, q, gain, sr); break;
            case 3: f[0].setHighPass (freq, q, sr); f[1].setHighPass (freq, 0.54f, sr); count = 2; break;
            case 4: f[0].setLowPass (freq, q, sr); f[1].setLowPass (freq, 0.54f, sr); count = 2; break;
            case 5: f[0].setNotch (freq, q, sr); break;
            case 6: f[0].setBandPass (freq, q, sr); break;
            default: f[0].setPeak (freq, q, gain, sr); break;
        }
    }
    float magnitudeDb (float hz, double sr) const
    {
        float db = 0.0f;
        for (int i = 0; i < count; ++i) db += f[i].magnitudeDb (hz, sr);
        return db;
    }
};

struct EqChannelBands
{
    EqBand b[8];
    Biquad state[8][2];   // processing state per band / stage
    int n = 0;
    void reset() { for (auto& st : state) for (auto& q : st) q.reset(); }
    void copyFrom (const EqBand* bands, int count)
    {
        n = count;
        for (int i = 0; i < count; ++i) { b[i] = bands[i]; for (int k = 0; k < bands[i].count; ++k) state[i][k].copyCoeffs (bands[i].f[k]); }
    }
    inline float process (float x)
    {
        for (int i = 0; i < n; ++i) for (int k = 0; k < b[i].count; ++k) x = state[i][k].process (x);
        return x;
    }
};

// ------------------------------------------------------------------ 3-Band EQ
// Low, Mid, High, Low Freq, Mid Freq, High Freq, Mid Q, Out,
// Low Type (Low Cut / Low Shelf / Peak), Mid Type (Peak / Notch / Band Pass),
// High Type (High Shelf / High Cut / Peak), Low Q, High Q
class Eq3 : public FxImpl
{
public:
    void prepare (double) override { tap.prepare (1024); reset(); }
    void reset() override { l.reset(); r.reset(); tap.reset(); }
    static void design (EqBand* b, const float* p, double sr)
    {
        static constexpr int lowT[3] = { 3, 1, 0 }, midT[3] = { 0, 5, 6 }, highT[3] = { 2, 4, 0 };
        b[0].design (lowT[juce::jlimit (0, 2, ci (p, 8))], p[3], p[0], p[11], sr);
        b[1].design (midT[juce::jlimit (0, 2, ci (p, 9))], p[4], p[1], p[6], sr);
        b[2].design (highT[juce::jlimit (0, 2, ci (p, 10))], p[5], p[2], p[12], sr);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        EqBand b[3];
        design (b, c.p, sr);
        l.copyFrom (b, 3); r.copyFrom (b, 3);
        const float out = dbToGain (c.p[7]);
        for (int i = 0; i < n; ++i) { L[i] = l.process (L[i]) * out; R[i] = r.process (R[i]) * out; }
        tap.push (L, R, n, sr, *taps);
    }
    bool responseDb (float hz, const float* p, int, const float*, double s, float& db) const override
    {
        EqBand b[3];
        design (b, p, s);
        db = b[0].magnitudeDb (hz, s) + b[1].magnitudeDb (hz, s) + b[2].magnitudeDb (hz, s) + p[7];
        return true;
    }
private:
    EqChannelBands l, r;
    SpectrumTap tap;
};

// ------------------------------------------------------------------ 8-Band EQ (parametric, shape data)
// Out, Scale, Analyzer | Stereo / Mid / Side
class Eq8 : public FxImpl
{
public:
    static constexpr int NB = Shape::kEqBands, S = Shape::kEqStride;
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        static constexpr float f[NB] = { 60, 150, 350, 800, 1800, 4000, 8000, 15000 };
        for (int b = 0; b < NB; ++b)
        {
            float* x = d + b * S;
            x[0] = 1.0f;
            x[1] = b == 0 ? 1.0f : (b == NB - 1 ? 2.0f : 0.0f);
            x[2] = f[b]; x[3] = 0.0f; x[4] = (b == 0 || b == NB - 1) ? 0.7f : 1.0f;
        }
    }
    void prepare (double) override { tap.prepare (1024); reset(); }
    void reset() override { l.reset(); r.reset(); tap.reset(); lastGen = -1; lastScale = -1.0f; }
    static int design (EqBand* out, const float* d, float scale, double sr)
    {
        int n = 0;
        for (int b = 0; b < NB; ++b)
        {
            const float* x = d + b * S;
            if (x[0] < 0.5f) continue;
            out[n++].design (juce::jlimit (0, Shape::kNumEqTypes - 1, (int) std::lround (x[1])), x[2], x[3] * scale, juce::jmax (0.1f, x[4]), sr);
        }
        return n;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        if (c.dataGen != lastGen || std::abs (c.p[1] - lastScale) > 1.0e-4f)
        {
            lastGen = c.dataGen; lastScale = c.p[1];
            EqBand b[NB];
            const int nb = design (b, c.data, c.p[1], sr);
            l.copyFrom (b, nb); r.copyFrom (b, nb);
        }
        const float out = dbToGain (c.p[0]);
        for (int i = 0; i < n; ++i)
        {
            float a = L[i], bb = R[i];
            if (c.mode == 0) { a = l.process (a); bb = r.process (bb); }
            else
            {
                float m = 0.5f * (a + bb), sd = 0.5f * (a - bb);
                if (c.mode == 1) m = l.process (m); else sd = l.process (sd);
                a = m + sd; bb = m - sd;
            }
            L[i] = a * out; R[i] = bb * out;
        }
        if (on (c, 2)) tap.push (L, R, n, sr, *taps);
    }
    bool responseDb (float hz, const float* p, int, const float* data, double s, float& db) const override
    {
        if (data == nullptr) return false;
        EqBand b[NB];
        const int nb = design (b, data, p[1], s);
        db = p[0];
        for (int i = 0; i < nb; ++i) db += b[i].magnitudeDb (hz, s);
        return true;
    }
private:
    EqChannelBands l, r;
    SpectrumTap tap;
    int lastGen = -1;
    float lastScale = -1.0f;
};

// ------------------------------------------------------------------ De-Esser
// Freq, Thresh, Range, Width, Listen | Split / Wide
class DeEsser : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { for (auto* b : { &det, &bL, &bR }) b->reset(); env = 0.0f; gs.reset(); }
    static float widthToQ (float oct) { const float t = std::pow (2.0f, oct * 0.5f); return t / (t * t - 1.0f); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float q = widthToQ (c.p[3]);
        det.setBandPass (c.p[0], q, sr); bL.copyCoeffs (det); bR.copyCoeffs (det);
        const float th = c.p[1], range = c.p[2];
        const bool listen = on (c, 4), wide = c.mode == 1;
        const float envA = msCoef (0.5f, sr), envR = msCoef (30.0f, sr);
        const float atk = msCoef (1.0f, sr), rel = msCoef (60.0f, sr);
        float last = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float s = det.process (0.5f * (L[i] + R[i]));
            env += (std::abs (s) > env ? envA : envR) * (std::abs (s) - env);
            const float over = gainToDb (env) - th;
            last = gs.process (over > 0.0f ? -juce::jmin (range, over * 0.75f) : 0.0f, atk, rel);
            const float g = dbToGain (last);
            const float sl = bL.process (L[i]), sr2 = bR.process (R[i]);
            if (listen)      { L[i] = sl; R[i] = sr2; }
            else if (wide)   { L[i] *= g; R[i] *= g; }
            else             { L[i] -= sl * (1.0f - g); R[i] -= sr2 * (1.0f - g); }
        }
        taps->set (0, last);
    }
    bool responseDb (float hz, const float* p, int, const float*, double s, float& db) const override
    {
        Biquad b; b.setBandPass (p[0], widthToQ (p[3]), s);
        db = b.magnitudeDb (hz, s);
        return true;
    }
private:
    Biquad det, bL, bR;
    float env = 0.0f;
    GainSmoother gs;
};

// ------------------------------------------------------------------ Hum Remover
// Fine, Harmonics, Q, Depth, Rumble | 50 Hz / 60 Hz
class HumRemover : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { for (auto& b : nl) b.reset(); for (auto& b : nr) b.reset(); hpL.reset(); hpR.reset(); key = -1.0f; }
    static int design (Biquad* b, Biquad& hp, const float* p, int mode, double sr)
    {
        const float base = (mode == 0 ? 50.0f : 60.0f) + p[0];
        const int harm = ci (p, 1) + 1;
        int used = 0;
        for (int h = 1; h <= harm; ++h)
        {
            const float f = base * (float) h;
            if (f > sr * 0.45) break;
            b[used++].setNotch (f, p[2] * std::sqrt ((float) h), sr);
        }
        hp.setHighPass (juce::jmax (10.0f, p[4]), 0.707f, sr);
        return used;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float k = c.p[0] * 1000.0f + c.p[1] * 100.0f + c.p[2] + c.p[4] * 0.001f + (float) c.mode * 7.0f;
        if (k != key)
        {
            key = k;
            used = design (nl, hpL, c.p, c.mode, sr);
            for (int i = 0; i < used; ++i) nr[i].copyCoeffs (nl[i]);
            hpR.copyCoeffs (hpL);
        }
        const float depth = c.p[3];
        const bool rumble = c.p[4] > 10.5f;
        for (int i = 0; i < n; ++i)
        {
            float l = L[i], r = R[i];
            if (rumble) { l = hpL.process (l); r = hpR.process (r); }
            float ln = l, rn = r;
            for (int h = 0; h < used; ++h) { ln = nl[h].process (ln); rn = nr[h].process (rn); }
            L[i] = l + (ln - l) * depth; R[i] = r + (rn - r) * depth;
        }
    }
    bool responseDb (float hz, const float* p, int mode, const float*, double s, float& db) const override
    {
        Biquad b[8], hp;
        const int u = design (b, hp, p, mode, s);
        float lin = 1.0f;
        for (int i = 0; i < u; ++i) lin *= dbToGain (b[i].magnitudeDb (hz, s));
        db = gainToDb (1.0f + (lin - 1.0f) * p[3]) + (p[4] > 10.5f ? hp.magnitudeDb (hz, s) : 0.0f);
        return true;
    }
private:
    Biquad nl[8], nr[8], hpL, hpR;
    int used = 0;
    float key = -1.0f;
};

// ------------------------------------------------------------------ Click Remover
// Sens, Width, Smooth, Listen | Vinyl / Digital
// Runs ~9 ms behind the input. A click is a high-passed spike well above the
// running HF level that dies away again quickly (real transients such as drum
// hits keep ringing, so they are left alone). The click's span (at most Width)
// is rebuilt by linear prediction: an AR model fitted to the audio before the
// gap predicts forwards, one fitted to the audio after it predicts backwards,
// and the two are crossfaded. Smooth low-passes the patch. Listen outputs what
// was removed.
class ClickRemover : public FxImpl
{
public:
    static constexpr float kMaxWidthMs = 3.0f;
    static constexpr int kOrder = 16, kFitBefore = 384, kFitAfter = 160, kMaxRegion = 200;
    void prepare (double s) override
    {
        lookahead = (int) std::ceil (kMaxWidthMs * 3.0f * 0.001f * (float) s) + 4;
        size = 2048;
        while (size < lookahead + kFitBefore + kMaxRegion + kFitAfter + 8) size *= 2;
        bl.assign ((size_t) size, 0.0f); br.assign ((size_t) size, 0.0f); hpAbs.assign ((size_t) size, 0.0f);
        reset();
    }
    void reset() override
    {
        std::fill (bl.begin(), bl.end(), 0.0f); std::fill (br.begin(), br.end(), 0.0f); std::fill (hpAbs.begin(), hpAbs.end(), 0.0f);
        hp.reset(); sl.reset(); sr2.reset(); w = 0; base = 1.0e-4f; pending = -1; regionStart = regionEnd = -1;
        fillReady = false; waiting = false; count = 0; timer = 0; rate = 0.0f; lastKey = -1.0f; holdOff = 0;
    }
    // AR coefficients (Levinson-Durbin) of x[0..n), then predict `len` samples following x
    static void predict (const float* x, int n, float* out, int len)
    {
        double r[kOrder + 1] {};
        for (int k = 0; k <= kOrder; ++k)
            for (int i = k; i < n; ++i) r[k] += (double) x[i] * x[i - k];
        r[0] *= 1.0001; r[0] += 1.0e-12;
        double a[kOrder + 1] {}, tmp[kOrder + 1] {};
        a[0] = 1.0;
        double err = r[0];
        for (int m = 1; m <= kOrder; ++m)
        {
            double acc = r[m];
            for (int j = 1; j < m; ++j) acc += a[j] * r[m - j];
            const double k = -acc / err;
            for (int j = 0; j <= m; ++j) tmp[j] = a[j];
            for (int j = 1; j < m; ++j) a[j] = tmp[j] + k * tmp[m - j];
            a[m] = k;
            err *= (1.0 - k * k);
            if (err <= 1.0e-15) break;
        }
        float hist[kOrder];
        for (int j = 0; j < kOrder; ++j) hist[j] = x[n - 1 - j];   // hist[0] = newest
        for (int t = 0; t < len; ++t)
        {
            double y = 0.0;
            for (int j = 1; j <= kOrder; ++j) y -= a[j] * hist[j - 1];
            const float v = juce::jlimit (-2.0f, 2.0f, (float) y);
            out[t] = v;
            for (int j = kOrder - 1; j > 0; --j) hist[j] = hist[j - 1];
            hist[0] = v;
        }
    }
    void buildFill (const std::vector<float>& b, float* fill)
    {
        const int len = (int) (regionEnd - regionStart);
        float before[kFitBefore], after[kFitAfter], fwd[kMaxRegion], bwd[kMaxRegion];
        for (int i = 0; i < kFitBefore; ++i) before[i] = b[(size_t) ((regionStart - kFitBefore + i) & (size - 1))];
        for (int i = 0; i < kFitAfter; ++i) after[i] = b[(size_t) ((regionEnd + kFitAfter - 1 - i) & (size - 1))];   // reversed
        predict (before, kFitBefore, fwd, len);
        predict (after, kFitAfter, bwd, len);
        for (int t = 0; t < len; ++t)
        {
            const float x = ((float) t + 0.5f) / (float) len;
            const float wv = x * x * (3.0f - 2.0f * x);
            fill[t] = fwd[t] * (1.0f - wv) + bwd[len - 1 - t] * wv;
        }
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float key = c.p[2] + (float) c.mode * 100000.0f;
        if (key != lastKey)
        {
            lastKey = key;
            hp.setHighPass (c.mode == 0 ? 2500.0f : 6000.0f, 0.707f, sr);
            sl.setCutoff (c.p[2], sr); sr2.setCutoff (c.p[2], sr);
        }
        const float thrMul = 3.0f + (1.0f - c.p[0]) * 14.0f;
        const int maxW = juce::jlimit (2, kMaxRegion - 8, (int) (c.p[1] * 0.001f * sr));
        const float baseCoef = msCoef (c.mode == 0 ? 50.0f : 20.0f, sr);
        const bool listen = on (c, 3);
        for (int i = 0; i < n; ++i)
        {
            // ---- analysis on the newest sample
            const long long now = w;
            const float h = std::abs (hp.process (0.5f * (L[i] + R[i])));
            bl[(size_t) (now & (size - 1))] = L[i]; br[(size_t) (now & (size - 1))] = R[i]; hpAbs[(size_t) (now & (size - 1))] = h;
            ++w;
            if (h < base * thrMul) base += baseCoef * (h - base);
            base = juce::jmax (base, 1.0e-5f);
            if (holdOff > 0) --holdOff;
            if (pending < 0 && ! waiting && holdOff == 0 && h > base * thrMul && h > 3.0e-3f) { pending = now; spikeLevel = h; }
            // confirm: within maxW the HF energy must have collapsed again
            if (pending >= 0 && now - pending >= maxW + 4)
            {
                long long last = pending;
                float tail = 0.0f;
                for (long long k = pending; k <= now; ++k)
                {
                    const float v = hpAbs[(size_t) (k & (size - 1))];
                    if (v > spikeLevel * 0.3f) last = k;
                    if (k > pending + maxW) tail = juce::jmax (tail, v);
                }
                if (tail < spikeLevel * 0.3f)
                {
                    regionStart = pending - 3;
                    regionEnd = juce::jmin (pending + maxW, last + 3);
                    waiting = true; fillReady = false;
                    ++count;
                }
                pending = -1;
                holdOff = maxW;
            }
            // enough audio after the click: rebuild it
            if (waiting && now >= regionEnd + kFitAfter)
            {
                buildFill (bl, fillL); buildFill (br, fillR);
                fillReady = true; waiting = false;
            }
            // ---- output, `lookahead` samples behind
            const long long out = now - lookahead;
            float ol = 0.0f, orr = 0.0f;
            if (out >= 0)
            {
                ol = bl[(size_t) (out & (size - 1))]; orr = br[(size_t) (out & (size - 1))];
                if (fillReady && out >= regionStart && out < regionEnd)
                {
                    const int t = (int) (out - regionStart);
                    const float rl = sl.lp (fillL[t]), rr = sr2.lp (fillR[t]);
                    if (listen) { ol -= rl; orr -= rr; } else { ol = rl; orr = rr; }
                }
                else
                {
                    sl.z = ol; sr2.z = orr;   // the patch's smoothing starts from the real signal
                    if (listen) { ol = 0.0f; orr = 0.0f; }
                }
                if (fillReady && out >= regionEnd) fillReady = false;
            }
            L[i] = ol; R[i] = orr;
        }
        timer += n;
        if (timer > (int) sr) { rate = (float) count * (float) sr / (float) timer; count = 0; timer = 0; }
        taps->set (0, rate);
    }
private:
    std::vector<float> bl, br, hpAbs;
    float fillL[kMaxRegion] {}, fillR[kMaxRegion] {};
    Biquad hp;
    OnePole sl, sr2;
    int lookahead = 436, size = 2048, holdOff = 0;
    long long w = 0, regionStart = -1, regionEnd = -1, pending = -1;
    bool fillReady = false, waiting = false;
    float base = 1.0e-4f, rate = 0.0f, lastKey = -1.0f, spikeLevel = 0.0f;
    int count = 0, timer = 0;
};

// ------------------------------------------------------------------ Humanizer
// Amount, Drift, Rate, Sens, Div, Repeat, Smooth, Seed | Hits / Steps / Free
// Small random level changes. Hits: a new random level at each detected hit
// (fast vs slow follower, like the Transient Shaper); Steps: one per tempo
// Div, optionally repeating every 1 / 2 / 4 bars so the feel loops with the
// music; Free: at random moments (60-400 ms apart). Drift adds a slow,
// smooth wander (Rate). Values are centred on 0 dB, so the average level
// stays put. Taps: m[0] gain dB, m[1] history write index, m[2..49] history.
class Humanizer : public FxImpl
{
public:
    static constexpr int kHist = 48;
    void prepare (double) override { reset(); }
    void reset() override
    {
        for (auto& e : env) e.reset();
        g = 0.0f; offDb = 0.0f; lastStep = -1; hits = 0; since = 1.0e9f; freeLeft = 0.0; freeCount = 0; driftT = 0.0; armed = true;
        histPos = 0; histTimer = 0;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float amount = c.p[0], drift = c.p[1], rate = c.p[2], sens = c.p[3];
        const int seed = (int) std::lround (c.p[7]);
        const float coef = msCoef (c.p[6], sr);
        env[0].set (0.5f, 60.0f, sr);
        env[1].set (25.0f, 300.0f, sr);   // slower release: a decaying hit falls under it and re-arms
        const double divB = divBeatsOf (*def, c.p, 4);
        static constexpr int kBars[4] = { 0, 1, 2, 4 };
        const int bars = kBars[juce::jlimit (0, 3, ci (c, 5))];
        const long long loopSteps = bars > 0 ? juce::jmax (1LL, (long long) std::llround ((double) bars * 4.0 / divB)) : 0;
        const double minGap = 0.03 * sr;
        for (int i = 0; i < n; ++i)
        {
            const float x = juce::jmax (std::abs (L[i]), std::abs (R[i]));
            if (c.mode == 0)
            {
                const float e0 = env[0].process (x), e1 = env[1].process (x);
                const float atk = gainToDb (e0) - gainToDb (e1);
                since += 1.0f;
                if (armed && atk > sens && gainToDb (e0) > -60.0f && since > minGap)
                {
                    ++hits; armed = false; since = 0.0f;
                    offDb = amount * humanizeHash (seed, hits);
                }
                if (! armed && atk < sens * 0.4f) armed = true;
            }
            else if (c.mode == 1)
            {
                const double beat = c.ppq + c.beatsPerSample * i;
                long long st = (long long) std::floor (beat / divB);
                if (loopSteps > 0) st = ((st % loopSteps) + loopSteps) % loopSteps;
                if (st != lastStep) { lastStep = st; offDb = amount * humanizeHash (seed, st); }
            }
            else
            {
                freeLeft -= 1.0;
                if (freeLeft <= 0.0)
                {
                    ++freeCount;
                    offDb = amount * humanizeHash (seed, freeCount);
                    freeLeft = (0.06 + 0.34 * (0.5 + 0.5 * humanizeHash (seed + 1000, freeCount))) * sr;
                }
            }
            // smooth value noise for the drift: cosine-interpolated random points, Rate per second
            driftT += rate / sr;
            const long long k = (long long) std::floor (driftT);
            const float f = (float) (driftT - (double) k), w = 0.5f - 0.5f * std::cos (f * kPi);
            const float dn = humanizeHash (seed + 500, k) * (1.0f - w) + humanizeHash (seed + 500, k + 1) * w;
            const float target = offDb + drift * dn;
            g += (target - g) * coef;
            const float lin = dbToGain (g);
            L[i] *= lin; R[i] *= lin;
            if (++histTimer >= (int) (0.02 * sr)) { histTimer = 0; taps->set (2 + histPos, g); histPos = (histPos + 1) % kHist; taps->set (1, (float) histPos); }
        }
        taps->set (0, g);
    }
private:
    EnvFollower env[2];
    float g = 0.0f, offDb = 0.0f, since = 1.0e9f;
    long long lastStep = -1, hits = 0, freeCount = 0;
    double freeLeft = 0.0, driftT = 0.0;
    bool armed = true;
    int histPos = 0, histTimer = 0;
};

// ------------------------------------------------------------------ Transient Shaper
// Attack, Sustain, Speed, Clip, Out, Release, Decay
// Two followers with the same release but different attacks: their gap (dB)
// is the attack measure, and Decay (the slow one's attack) sets how long the
// attack boost / cut lasts after each hit. Sustain compares a tail follower
// (release = Release) with a short one: how far the signal sits under its
// own tail is the sustain measure.
class TransientShaper : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { for (auto& e : env) e.reset(); g = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float k = std::pow (4.0f, 1.0f - 2.0f * c.p[2]);   // speed: 4x slower .. 4x faster
        // the attack measure lasts until the slow follower catches up with the
        // fast one, so Decay is the slow follower's attack time; both release
        // slower than that (and slower than a low note's cycle: no ripple)
        const float decay = c.p[6], rel = juce::jmax (120.0f * k, decay * 1.5f);
        env[0].set (0.5f * k, rel, sr);           // fast attack
        env[1].set (decay, rel, sr);              // slow attack = Decay
        env[2].set (0.5f * k, c.p[5], sr);        // long release (tail follower)
        env[3].set (0.5f * k, 30.0f * k, sr);     // short release
        const float atkAmt = c.p[0], susAmt = c.p[1];
        const bool clip = on (c, 3);
        const float out = dbToGain (c.p[4]);
        const float smooth = msCoef (c.mode == 0 ? 0.3f : 6.0f, sr);
        for (int i = 0; i < n; ++i)
        {
            const float x = juce::jmax (std::abs (L[i]), std::abs (R[i]));
            const float e0 = env[0].process (x), e1 = env[1].process (x), e2 = env[2].process (x), e3 = env[3].process (x);
            const float atk = juce::jmax (0.0f, gainToDb (e0) - gainToDb (e1));
            // sustain: how far the signal sits under its own long tail, up to 36 dB
            const float sus = juce::jlimit (0.0f, 36.0f, gainToDb (e2) - gainToDb (e3));
            // each part saturates towards its full range: Attack +-100% = +-12 dB on
            // a sharp hit, Sustain +100% = +12 dB / -100% = -24 dB on a long tail
            const float atkDb = atkAmt * 12.0f * (1.0f - std::exp (-atk / 6.0f));
            const float susDb = susAmt * (susAmt > 0.0f ? 12.0f : 24.0f) * (1.0f - std::exp (-sus / 8.0f));
            const float target = juce::jlimit (-40.0f, 12.0f, atkDb + susDb);
            g += (target - g) * smooth;
            const float lin = dbToGain (g) * out;
            float l = L[i] * lin, r = R[i] * lin;
            if (clip) { l = fastTanh (l); r = fastTanh (r); }
            L[i] = l; R[i] = r;
        }
        taps->set (0, g);
    }
private:
    EnvFollower env[4];
    float g = 0.0f;
};

// ------------------------------------------------------------------ Ducker
// Depth, Div, Smooth, Length, Thresh, SC HP | Sync / Ext
// Shape data: the duck curve (CurvePoints, y = duck amount 0..1 over one
// division in Sync mode, over Length after each trigger in Ext mode).
class Ducker : public FxImpl
{
public:
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        CurvePoints::set (d, { { 0.0f, 1.0f, 0.0f }, { 0.04f, 1.0f, 0.55f }, { 0.6f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } });
    }
    void prepare (double) override { reset(); }
    void reset() override { since = 1.0e9; hp.reset(); env = 0.0f; armed = true; g = 1.0f; lastHp = -1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float depth = c.p[0];
        const bool sync = c.mode == 0;
        const double divB = divBeatsOf (*def, c.p, 1);
        const double lenS = juce::jmax (1.0, (double) c.p[3] * 0.001 * sr);
        if (std::abs (c.p[5] - lastHp) > 0.01f) { hp.setHighPass (c.p[5], 0.707f, sr); lastHp = c.p[5]; }
        const float th = dbToGain (c.p[4]);
        const float envR = msCoef (30.0f, sr), sm = msCoef (c.p[2], sr);
        float pos = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            if (sync)
            {
                const double beat = c.ppq + c.beatsPerSample * i;
                pos = (float) (beat / divB - std::floor (beat / divB));
            }
            else
            {
                float k = c.scL != nullptr ? 0.5f * (c.scL[i] + c.scR[i]) : 0.5f * (L[i] + R[i]);
                if (c.p[5] > 21.0f) k = hp.process (k);
                const float a = std::abs (k);
                env = a > env ? a : env + (a - env) * envR;
                if (env > th && armed) { since = 0.0; armed = false; }
                if (env < th * 0.5f) armed = true;
                since += 1.0;
                pos = (float) juce::jmin (1.0, since / lenS);
            }
            const float amt = (sync || since < lenS) ? clamp01 (CurvePoints::eval (c.data, pos)) : 0.0f;
            g += ((1.0f - depth * amt) - g) * sm;
            L[i] *= g; R[i] *= g;
        }
        taps->set (0, g); taps->set (1, pos);
    }
private:
    double since = 1.0e9;
    Biquad hp;
    float env = 0.0f, g = 1.0f, lastHp = -1.0f;
    bool armed = true;
};

// ------------------------------------------------------------------ Trance Gate
// Div, Steps, Depth, Attack, Release, Swing, Gate
// Shape data: 32 step levels (0..1).
class TranceGate : public FxImpl
{
public:
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        static constexpr int pat[16] = { 1,0,1,1, 0,1,1,0, 1,1,0,1, 1,0,1,1 };
        for (int s = 0; s < Shape::kSteps; ++s) d[s] = (float) pat[s % 16];
    }
    void prepare (double) override { reset(); }
    void reset() override { g = 1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double stepB = divBeatsOf (*def, c.p, 0);
        const int steps = juce::jlimit (1, Shape::kSteps, ci (c, 1));
        const float depth = c.p[2], atk = msCoef (c.p[3], sr), rel = msCoef (c.p[4], sr), swing = c.p[5], gate = c.p[6];
        int lastStep = 0;
        for (int i = 0; i < n; ++i)
        {
            const double beat = c.ppq + c.beatsPerSample * i;
            const double pos = beat / stepB;
            long long stepAbs = (long long) std::floor (pos);
            double frac = pos - (double) stepAbs;
            if ((stepAbs & 1) == 1)
            {
                if (frac < swing) { --stepAbs; frac += 1.0; }
                else frac = (frac - swing) / (1.0 - swing);
            }
            else frac = frac / (1.0 + swing);
            const int step = (int) (((stepAbs % steps) + steps) % steps);
            const float lvl = clamp01 (c.data[step]);
            const float target = frac < gate ? 1.0f - depth * (1.0f - lvl) : 1.0f - depth;
            g += (target > g ? atk : rel) * (target - g);
            L[i] *= g; R[i] *= g;
            lastStep = step;
        }
        taps->set (0, g); taps->set (1, (float) lastStep);
    }
private:
    float g = 1.0f;
};

// ------------------------------------------------------------------ Gain / utility
// Gain, Pan, Width, Phase L, Phase R, DC Cut | Stereo / Mono / Swap
class GainFx : public FxImpl
{
public:
    void prepare (double) override { reset(); }
    void reset() override { dcL.reset(); dcR.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float g = dbToGain (c.p[0]);
        const float pan = c.p[1];
        const float gl = pan > 0.0f ? 1.0f - pan : 1.0f, gr = pan < 0.0f ? 1.0f + pan : 1.0f;
        const float pl = on (c, 3) ? -1.0f : 1.0f, pr = on (c, 4) ? -1.0f : 1.0f;
        const bool dc = on (c, 5);
        for (int i = 0; i < n; ++i)
        {
            float l = L[i] * pl, r = R[i] * pr;
            if (dc) { l = dcL.process (l, 0.9993f); r = dcR.process (r, 0.9993f); }
            if (c.mode == 1) { const float m = 0.5f * (l + r); l = r = m; }
            else if (c.mode == 2) std::swap (l, r);
            applyWidth (l, r, c.p[2]);
            L[i] = l * g * gl; R[i] = r * g * gr;
        }
    }
private:
    DcBlocker dcL, dcR;
};

} // namespace

std::unique_ptr<FxImpl> makeEqVolumeFx (const juce::String& n)
{
    if (n == "3-Band EQ")        return std::make_unique<Eq3>();
    if (n == "8-Band EQ")        return std::make_unique<Eq8>();
    if (n == "De-Esser")         return std::make_unique<DeEsser>();
    if (n == "Hum Remover")      return std::make_unique<HumRemover>();
    if (n == "Click Remover")    return std::make_unique<ClickRemover>();
    if (n == "Transient Shaper") return std::make_unique<TransientShaper>();
    if (n == "Humanizer")        return std::make_unique<Humanizer>();
    if (n == "Ducker")           return std::make_unique<Ducker>();
    if (n == "Trance Gate")      return std::make_unique<TranceGate>();
    if (n == "Gain")             return std::make_unique<GainFx>();
    return nullptr;
}

} // namespace efx
