#pragma once
#include "../Effects.h"

// Helpers shared by the per-category effect files (fx/*.cpp). Each effect is
// an FxImpl subclass; c.p[k] holds the physical value of control k in the
// order fx_spec.py lists them (choices arrive as their index).

namespace efx
{

// choice control as int
inline int ci (const FxContext& c, int k) { return (int) std::lround (c.p[k]); }
inline int ci (const float* p, int k) { return (int) std::lround (p[k]); }
inline bool on (const FxContext& c, int k) { return ci (c, k) != 0; }

// beats of a tempo-division choice control
inline double divBeatsOf (const EffectDef& d, const float* p, int k)
{
    const auto& s = d.p[k];
    const int idx = juce::jlimit (0, juce::jmax (0, s.numChoices - 1), ci (p, k));
    return (s.choices != nullptr) ? divisionBeats (s.choices[idx]) : 1.0;
}

// level-independent soft clip used as a safety stage inside feedback loops
inline float loopSat (float x) { return fastTanh (x * 0.7f) / 0.7f; }

// transparent up to 0 dBFS, then a soft knee that never exceeds +6 dB
inline float safeCeiling (float x)
{
    const float a = std::abs (x);
    return a <= 1.0f ? x : std::copysign (1.0f + fastTanh (a - 1.0f), x);
}

// mid/side width (0 = mono, 1 = unchanged, 2 = double side)
inline void applyWidth (float& l, float& r, float width)
{
    const float m = 0.5f * (l + r), s = 0.5f * (l - r) * width;
    l = m + s; r = m - s;
}

// constant-power pan gains for p in -1..1 (unity at centre)
inline void panGains (float p, float& gl, float& gr)
{
    const float a = (juce::jlimit (-1.0f, 1.0f, p) + 1.0f) * 0.25f * kPi;
    gl = std::cos (a) * 1.41421356f;
    gr = std::sin (a) * 1.41421356f;
}

// tilt EQ around ~800 Hz: amount -1..1 -> -6..+6 dB high / opposite low
struct Tilt
{
    Biquad lo, hi;
    float last = 1.0e9f;
    void reset() { lo.reset(); hi.reset(); }
    void set (float amt, double sr)
    {
        if (std::abs (amt - last) < 1.0e-4f) return;
        last = amt;
        lo.setLowShelf (800.0f, 0.6f, -6.0f * amt, sr);
        hi.setHighShelf (800.0f, 0.6f, 6.0f * amt, sr);
    }
    inline float process (float x) { return hi.process (lo.process (x)); }
};

// stereo pair helpers
template <typename T> struct Stereo { T l, r; void reset() { l.reset(); r.reset(); } };

// compressor static curve: gain change (dB, <= 0) for detector level x (dB)
inline float compCurve (float x, float th, float ratio, float knee)
{
    const float slope = 1.0f / juce::jmax (1.0f, ratio) - 1.0f;
    const float over = x - th;
    if (knee > 0.0f && std::abs (over) < knee * 0.5f)
    {
        const float t = over + knee * 0.5f;
        return slope * t * t / (2.0f * knee);
    }
    return over > 0.0f ? slope * over : 0.0f;
}

// smoother in the dB domain with separate attack/release coefficients
struct GainSmoother
{
    float db = 0.0f;
    void reset() { db = 0.0f; }
    inline float process (float target, float atkCoef, float relCoef)
    {
        db += (target < db ? atkCoef : relCoef) * (target - db);
        return db;
    }
};

// sliding-window minimum over the last `size` values (monotone deque)
struct SlidingMin
{
    std::vector<float> val; std::vector<long long> idx;
    long long n = 0; int head = 0, tail = 0, cap = 2, size = 1;
    void prepare (int capacity) { cap = juce::jmax (2, capacity + 1); val.assign ((size_t) cap, 1.0f); idx.assign ((size_t) cap, 0); reset(); }
    void reset() { n = 0; head = tail = 0; }
    void setSize (int s) { size = juce::jlimit (1, cap - 1, s); }
    inline float push (float v)
    {
        const long long i = n++;
        while (tail > head && val[(size_t) ((tail - 1) % cap)] >= v) --tail;
        val[(size_t) (tail % cap)] = v; idx[(size_t) (tail % cap)] = i; ++tail;
        while (idx[(size_t) (head % cap)] <= i - size) ++head;
        if (head > 1000000) { const int sh = head - head % cap; head -= sh; tail -= sh; }
        return val[(size_t) (head % cap)];
    }
};

// output guard: instant-attack peak limiter (1 ms hold, 60 ms release) that
// keeps a stage from clipping, e.g. after generous make-up gain
struct ClipGuard
{
    SlidingMin hold;
    float g = 1.0f, rel = 0.001f, reduction = 0.0f;
    void prepare (double sr) { hold.prepare ((int) (0.002 * sr) + 2); hold.setSize ((int) (0.001 * sr) + 1); rel = msCoef (60.0f, sr); reset(); }
    void reset() { hold.reset(); g = 1.0f; reduction = 0.0f; }
    inline void process (float& l, float& r, float ceil)
    {
        const float pk = juce::jmax (std::abs (l), std::abs (r));
        const float t = hold.push (pk > ceil ? ceil / pk : 1.0f);
        g = t < g ? t : g + (t - g) * rel;
        l = juce::jlimit (-ceil, ceil, l * g);
        r = juce::jlimit (-ceil, ceil, r * g);
        reduction = juce::jmin (reduction, gainToDb (g));
    }
    float takeReduction() { const float v = reduction; reduction = 0.0f; return v; }
};

// FFT spectrum of the mono sum into VizTaps::spec (log bands 20 Hz..20 kHz)
struct SpectrumTap
{
    static constexpr int kOrder = 11, kN = 1 << kOrder;
    juce::dsp::FFT fft { kOrder };
    std::vector<float> ring, work, win;
    int w = 0, hop = 0, hopSize = 1024;
    float sm[VizTaps::kSpec] {};
    void prepare (int hopSamples = 1024)
    {
        hopSize = hopSamples;
        ring.assign (kN, 0.0f); work.assign (kN * 2, 0.0f); win.resize (kN);
        for (int i = 0; i < kN; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) kN);
        for (auto& v : sm) v = -120.0f;
    }
    void reset() { std::fill (ring.begin(), ring.end(), 0.0f); w = hop = 0; for (auto& v : sm) v = -120.0f; }
    // returns true when a new frame was analysed
    template <typename OnFrame>
    void push (const float* L, const float* R, int n, double sr, VizTaps& t, float release, float tilt, bool peak, OnFrame&& onFrame)
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) w] = 0.5f * (L[i] + R[i]);
            w = (w + 1) & (kN - 1);
            if (++hop >= hopSize) { hop = 0; analyse (sr, t, release, tilt, peak); onFrame (work.data()); }
        }
    }
    void push (const float* L, const float* R, int n, double sr, VizTaps& t, float release = 0.25f, float tilt = 3.0f, bool peak = false)
    {
        push (L, R, n, sr, t, release, tilt, peak, [] (const float*) {});
    }
    void analyse (double sr, VizTaps& t, float release, float tilt, bool peak)
    {
        for (int i = 0; i < kN; ++i) work[(size_t) i] = ring[(size_t) ((w + i) & (kN - 1))] * win[(size_t) i];
        std::fill (work.begin() + kN, work.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        const float binHz = (float) sr / (float) kN;
        for (int b = 0; b < VizTaps::kSpec; ++b)
        {
            const float f0 = 20.0f * std::pow (1000.0f, (float) b / VizTaps::kSpec);
            const float f1 = 20.0f * std::pow (1000.0f, (float) (b + 1) / VizTaps::kSpec);
            const float k0 = f0 / binHz, k1 = f1 / binHz;
            float mag = 0.0f;
            if (k1 - k0 < 1.0f)
            {
                const float k = 0.5f * (k0 + k1);
                const int i0 = juce::jlimit (0, kN / 2 - 1, (int) k);
                const float fr = k - (float) i0;
                mag = work[(size_t) i0] * (1.0f - fr) + work[(size_t) i0 + 1] * fr;
            }
            else
                for (int k = juce::jmax (0, (int) k0); k <= juce::jmin (kN / 2, (int) k1); ++k) mag = juce::jmax (mag, work[(size_t) k]);
            const float fc = std::sqrt (f0 * f1);
            const float db = gainToDb (mag * 4.0f / (float) kN) + tilt * std::log2 (fc / 1000.0f);
            sm[b] = db > sm[b] ? sm[b] + (db - sm[b]) * 0.8f : sm[b] + (db - sm[b]) * release;
            t.spec[(size_t) b].store (sm[b]);
            const float pk = t.specPeak[(size_t) b].load();
            t.specPeak[(size_t) b].store (peak ? juce::jmax (sm[b], pk - 0.25f) : sm[b]);
        }
    }
};

// circular history buffer for the buffer-based time effects
struct History
{
    std::vector<float> l, r;
    int size = 0;
    long long w = 0;   // total samples written
    void prepare (double sr, float seconds) { size = juce::jmax (64, (int) (sr * seconds)); l.assign ((size_t) size, 0.0f); r.assign ((size_t) size, 0.0f); w = 0; }
    void reset() { std::fill (l.begin(), l.end(), 0.0f); std::fill (r.begin(), r.end(), 0.0f); w = 0; }
    inline void write (float a, float b) { const size_t i = (size_t) (w % size); l[i] = a; r[i] = b; ++w; }
    // absolute position (samples since start); clamps to what is still held
    inline void read (double pos, float& a, float& b) const
    {
        if (pos < (double) (w - size + 2)) pos = (double) (w - size + 2);
        if (pos > (double) (w - 1)) pos = (double) (w - 1);
        if (pos < 0.0) { a = b = 0.0f; return; }
        const long long i0 = (long long) pos;
        const float fr = (float) (pos - (double) i0);
        const size_t a0 = (size_t) (i0 % size), a1 = (size_t) ((i0 + 1) % size);
        a = l[a0] + (l[a1] - l[a0]) * fr;
        b = r[a0] + (r[a1] - r[a0]) * fr;
    }
};

// Hann window value for t in 0..1
inline float hann (float t) { return 0.5f - 0.5f * std::cos (kTwoPi * clamp01 (t)); }

// grain windows: 0 Hann, 1 triangle, 2 Tukey (flat top), 3 exponential decay
inline float grainWindow (int type, float t)
{
    t = clamp01 (t);
    switch (type)
    {
        case 1: return 1.0f - std::abs (2.0f * t - 1.0f);
        case 2: return t < 0.25f ? hann (t * 2.0f) : (t > 0.75f ? hann ((t - 0.5f) * 2.0f) : 1.0f);
        case 3: return juce::jmin (1.0f, t * 40.0f) * std::exp (-5.0f * t);
        default: return hann (t);
    }
}

// random pitch offset (semitones, -range..range) snapped by a quantize mode:
// 0 free, 1 semitones, 2 octaves, 3 fifths / octaves
inline float quantizedDetune (Rng& r, float rangeSemis, int quantize)
{
    float v = r.bipolar() * rangeSemis;
    switch (quantize)
    {
        case 1: return std::round (v);
        case 2: return 12.0f * std::round (v / 12.0f);
        case 3: { static constexpr float steps[] = { -12.0f, -5.0f, 0.0f, 7.0f, 12.0f, 19.0f };
                  const int n = (int) std::lround ((v / juce::jmax (1.0f, rangeSemis) * 0.5f + 0.5f) * 5.0f);
                  return rangeSemis < 0.5f ? 0.0f : steps[juce::jlimit (0, 5, n)]; }
        default: return v;
    }
}

// granular pitch voice: overlapping grains read from a 1 s mono buffer at a
// chosen set of pitch ratios (one ratio per grain, round robin)
struct GrainPitch
{
    static constexpr int kG = 24;
    struct G { double pos = 0.0, inc = 1.0; int len = 0, age = 0; bool on = false; };
    std::vector<float> buf;
    int size = 0;
    long long w = 0;
    G g[kG];
    Rng rng;
    double spawn = 0.0;
    int next = 0;
    void prepare (double sr, juce::uint32 seed) { size = (int) sr + 8; buf.assign ((size_t) size, 0.0f); rng.seed (seed); reset(); }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; for (auto& x : g) x.on = false; spawn = 0.0; }
    inline float read (double pos) const
    {
        // only what is still in the buffer: never before the first sample
        // written (early on w - size is negative) and never NaN
        const double lo = (double) juce::jmax (0LL, w - size + 2), hi = (double) (w - 1);
        if (! (pos >= lo && pos <= hi)) return 0.0f;
        const long long i0 = (long long) pos;
        const float fr = (float) (pos - (double) i0);
        const float a = buf[(size_t) (i0 % size)], b = buf[(size_t) ((i0 + 1) % size)];
        return a + (b - a) * fr;
    }
    // ratios: n pitch ratios; grainLen samples; rate grains per second; spray 0..1
    inline float process (float x, const float* ratios, int n, int grainLen, float rate, float spray, double sr)
    {
        buf[(size_t) (w % size)] = x; ++w;
        spawn += rate / sr;
        if (spawn >= 1.0 && n > 0)
        {
            spawn -= 1.0;
            for (auto& gr : g)
                if (! gr.on)
                {
                    const double ratio = ratios[next++ % n];
                    const double reach = (double) grainLen * juce::jmax (1.0, ratio) + 4.0;
                    gr.pos = (double) w - reach - spray * rng.uni() * 0.25 * sr;
                    gr.inc = ratio; gr.len = grainLen; gr.age = 0; gr.on = true;
                    break;
                }
        }
        float y = 0.0f;
        for (auto& gr : g)
        {
            if (! gr.on) continue;
            y += read (gr.pos) * hann ((float) gr.age / (float) gr.len);
            gr.pos += gr.inc;
            if (++gr.age >= gr.len) gr.on = false;
        }
        return y / std::sqrt (juce::jmax (1.0f, rate * (float) grainLen / (float) sr));
    }
};

// factories per category file: return nullptr when the name isn't theirs
std::unique_ptr<FxImpl> makeAnalysisFx (const juce::String& name);
std::unique_ptr<FxImpl> makeDynamicsFx (const juce::String& name);
std::unique_ptr<FxImpl> makeEqVolumeFx (const juce::String& name);
std::unique_ptr<FxImpl> makeFilterFx (const juce::String& name);
std::unique_ptr<FxImpl> makeModulationFx (const juce::String& name);
std::unique_ptr<FxImpl> makeStereoPitchFx (const juce::String& name);
std::unique_ptr<FxImpl> makeDriveFx (const juce::String& name);
std::unique_ptr<FxImpl> makeSpaceFx (const juce::String& name);
std::unique_ptr<FxImpl> makeSpectralTimeFx (const juce::String& name);

} // namespace efx
