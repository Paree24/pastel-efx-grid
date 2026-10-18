#include "FxCommon.h"

// Modulation. Meter conventions (VizTaps::m):
//   m[0] LFO phase 0..1 (or position), m[1] current mod value (-1..1 / Hz)
//   Doppler: m[0], m[1] source x/y normalised, m[2] distance m
//   Formant Shifter: m[0] detected Hz

namespace efx
{
namespace
{

// Base for the LFO effects: shape (incl. a drawn custom shape kept in the
// slot's shape data), morph, freeze and optional tempo sync.
class LfoFx : public FxImpl
{
public:
    bool usesData() const override { return true; }
    void defaultData (float* d) const override { LfoShapes::defaultCustom (d); }
protected:
    ShapeLfo lfo;
    // phase increment per sample (0 when frozen); a synced LFO follows the host position
    double lfoInc (const FxContext& c, int iSync, int iRate, int iDiv, int iFreeze)
    {
        if (iFreeze >= 0 && on (c, iFreeze)) return 0.0;
        if (iSync >= 0 && on (c, iSync))
        {
            const double beats = divBeatsOf (*def, c.p, iDiv);
            lfo.phase = c.ppq / beats - std::floor (c.ppq / beats);
            return c.beatsPerSample / beats;
        }
        return c.p[iRate] / c.sr;
    }
    float shapeValue (const FxContext& c, int iShape, int iMorph, double offset = 0.0) const
    {
        return lfo.value (juce::jlimit (0, LfoShapes::kNum - 1, ci (c, iShape)), clamp01 (c.p[iMorph]), c.data, offset);
    }
    void publishPhase() { taps->set (0, (float) lfo.phase); }
};

// ------------------------------------------------------------------ Autopan
// Sync, Rate, Div, Depth, Shape, Smooth, Morph, Freeze
class Autopan : public LfoFx
{
public:
    void prepare (double) override { reset(); }
    void reset() override { lfo.reset(); sm = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double inc = lfoInc (c, 0, 1, 2, 7);
        const float depth = c.p[3];
        const float smooth = c.p[5] < 0.05f ? 1.0f : msCoef (c.p[5], sr);
        for (int i = 0; i < n; ++i)
        {
            sm += (shapeValue (c, 4, 6) - sm) * smooth;
            lfo.advance (inc);
            float gl, gr;
            panGains (sm * depth, gl, gr);
            L[i] *= gl; R[i] *= gr;
        }
        publishPhase(); taps->set (1, sm * depth);
    }
private:
    float sm = 0.0f;
};

// ------------------------------------------------------------------ Chorus
// Rate, Depth, Delay, Voices, Spread, Feedback, Tone, Shape, Morph, Drift, Freeze
class Chorus : public LfoFx
{
public:
    static constexpr int kV = 16;
    void prepare (double s) override { dl.prepare (s, 0.12f); dr.prepare (s, 0.12f); reset(); }
    void reset() override
    {
        dl.reset(); dr.reset(); lfo.reset(); fbl = fbr = 0.0f; tl.reset(); tr.reset();
        for (int v = 0; v < kV; ++v) ph[v] = (double) v / kV;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float depth = c.p[1], base = c.p[2] * 0.001f * (float) sr;
        const int voices = juce::jlimit (1, kV, ci (c, 3));
        const float spread = c.p[4], fb = c.p[5], drift = c.p[9];
        const int shape = juce::jlimit (0, LfoShapes::kNum - 1, ci (c, 7));
        const float morph = c.p[8];
        tl.setCutoff (c.p[6], sr); tr.setCutoff (c.p[6], sr);
        const float norm = 0.7071f / std::sqrt ((float) voices);
        const bool frozen = on (c, 10);
        const double inc = frozen ? 0.0 : c.p[0] / sr;
        float gl[kV], gr[kV], rate[kV];
        for (int v = 0; v < voices; ++v)
        {
            const float pan = voices > 1 ? ((float) v / (float) (voices - 1) * 2.0f - 1.0f) * spread : 0.0f;
            panGains (pan, gl[v], gr[v]);
            // each voice drifts at its own, slightly different rate
            rate[v] = 1.0f + drift * 0.45f * std::sin ((float) v * 2.399f);
        }
        for (int i = 0; i < n; ++i)
        {
            dl.write (L[i] + fbl * fb); dr.write (R[i] + fbr * fb);
            float wl = 0.0f, wr = 0.0f;
            for (int v = 0; v < voices; ++v)
            {
                const float mv = LfoShapes::value (shape, ph[v], c.data, lfo.held, lfo.next);
                const float mv2 = morph > 0.001f ? LfoShapes::value ((shape + 1) % LfoShapes::kNum, ph[v], c.data, lfo.held, lfo.next) : mv;
                const float m = lerp (mv, mv2, morph);
                const float y = (v & 1 ? dr : dl).readCubic (base * (1.0f + depth * 0.45f * m) + 2.0f);
                wl += y * gl[v]; wr += y * gr[v];
                ph[v] += inc * rate[v]; ph[v] -= std::floor (ph[v]);
            }
            lfo.advance (inc);
            wl *= norm; wr *= norm;
            fbl = loopSat (wl); fbr = loopSat (wr);
            L[i] = tl.lp (wl); R[i] = tr.lp (wr);
        }
        taps->set (0, (float) ph[0]);
    }
private:
    DelayLine dl, dr;
    OnePole tl, tr;
    double ph[kV] {};
    float fbl = 0.0f, fbr = 0.0f;
};

// ------------------------------------------------------------------ Flanger
// Rate, Depth, Manual, Feedback, Spread, Shape, Morph, Freeze, Sync, Div
class Flanger : public LfoFx
{
public:
    void prepare (double s) override { dl.prepare (s, 0.05f); dr.prepare (s, 0.05f); reset(); }
    void reset() override { dl.reset(); dr.reset(); lfo.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float manual = c.p[2] * 0.001f * (float) sr, sweep = c.p[1] * 0.006f * (float) sr;
        const float fb = c.p[3];
        const double off = c.p[4] / 360.0;
        const double inc = lfoInc (c, 8, 0, 9, 7);
        for (int i = 0; i < n; ++i)
        {
            const float ml = 0.5f + 0.5f * shapeValue (c, 5, 6), mr = 0.5f + 0.5f * shapeValue (c, 5, 6, off);
            lfo.advance (inc);
            const float yl = dl.readCubic (manual + sweep * ml + 2.0f), yr = dr.readCubic (manual + sweep * mr + 2.0f);
            dl.write (L[i] + loopSat (yl * fb)); dr.write (R[i] + loopSat (yr * fb));
            L[i] = yl; R[i] = yr;
        }
        publishPhase();
    }
private:
    DelayLine dl, dr;
};

// ------------------------------------------------------------------ Phaser
// Rate, Depth, Center, Feedback, Stages, Spread, Shape, Morph, Freeze, Sync, Div
class Phaser : public LfoFx
{
public:
    static constexpr int kStages[5] = { 2, 4, 6, 8, 12 };
    void prepare (double) override { reset(); }
    void reset() override { for (auto& a : al) a.reset(); for (auto& a : ar) a.reset(); lfo.reset(); fbl = fbr = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int st = kStages[juce::jlimit (0, 4, ci (c, 4))];
        const float depth = c.p[1], centre = c.p[2], fb = c.p[3];
        const double off = c.p[5] / 360.0;
        const double inc = lfoInc (c, 9, 0, 10, 8);
        float fcl = centre;
        for (int i = 0; i < n; ++i)
        {
            fcl = centre * std::pow (2.0f, depth * 2.0f * shapeValue (c, 6, 7));
            const float fcr = centre * std::pow (2.0f, depth * 2.0f * shapeValue (c, 6, 7, off));
            lfo.advance (inc);
            const float cl = Allpass1::coef (fcl, sr), cr = Allpass1::coef (fcr, sr);
            float l = L[i] + fbl * fb, r = R[i] + fbr * fb;
            for (int s2 = 0; s2 < st; ++s2) { l = al[s2].process (l, cl); r = ar[s2].process (r, cr); }
            fbl = loopSat (l); fbr = loopSat (r);
            L[i] = l; R[i] = r;
        }
        publishPhase(); taps->set (1, fcl);
    }
private:
    Allpass1 al[12], ar[12];
    float fbl = 0.0f, fbr = 0.0f;
};

// ------------------------------------------------------------------ Wah
// Pedal, Sens, Rate, Low, High, Q, Release, Drive, Shape, Morph, Freeze | Auto / LFO / Pedal
class Wah : public LfoFx
{
public:
    void prepare (double s) override { fl.prepare (s); fr.prepare (s); reset(); }
    void reset() override { fl.reset(); fr.reset(); env.reset(); lfo.reset(); pos = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        env.set (4.0f, c.p[6], sr);
        const float lo = c.p[3], hi = juce::jmax (c.p[3] * 1.2f, c.p[4]), q = c.p[5];
        const float drive = c.p[7];
        const float posCoef = msCoef (8.0f, sr);
        const double inc = lfoInc (c, -1, 2, -1, 10);
        for (int i = 0; i < n; ++i)
        {
            float target;
            if (c.mode == 0) target = clamp01 (env.process (0.5f * (L[i] + R[i])) * c.p[1] * 10.0f);
            else if (c.mode == 1) { target = 0.5f + 0.5f * shapeValue (c, 8, 9); lfo.advance (inc); }
            else target = c.p[0];
            pos += (target - pos) * posCoef;
            const float fc = lo * std::pow (hi / lo, pos);
            auto chan = [&] (Svf& f, float x)
            {
                if (drive > 0.001f) { const float g = 1.0f + drive * 6.0f; x = fastTanh (x * g) / std::sqrt (g); }
                float lp, bp, hp;
                f.processQ (x, fc, q, lp, bp, hp);
                return bp / q * std::sqrt (q) * 1.1f + lp * 0.15f;
            };
            L[i] = chan (fl, L[i]); R[i] = chan (fr, R[i]);
        }
        taps->set (0, pos); taps->set (1, lo * std::pow (hi / lo, pos));
    }
private:
    Svf fl, fr;
    EnvFollower env;
    float pos = 0.0f;
};

// ------------------------------------------------------------------ Tremolo
// Sync, Rate, Div, Depth, Shape, Stereo, Smooth, Morph, Freeze
class Tremolo : public LfoFx
{
public:
    void prepare (double) override { reset(); }
    void reset() override { lfo.reset(); sl = sr2 = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double inc = lfoInc (c, 0, 1, 2, 8);
        const float depth = c.p[3];
        const double off = c.p[5] / 360.0;
        const float smooth = c.p[6] < 0.05f ? 1.0f : msCoef (c.p[6], sr);
        for (int i = 0; i < n; ++i)
        {
            sl += (shapeValue (c, 4, 7) - sl) * smooth;
            sr2 += (shapeValue (c, 4, 7, off) - sr2) * smooth;
            lfo.advance (inc);
            L[i] *= 1.0f - depth * (0.5f - 0.5f * sl);
            R[i] *= 1.0f - depth * (0.5f - 0.5f * sr2);
        }
        publishPhase(); taps->set (1, sl);
    }
private:
    float sl = 0.0f, sr2 = 0.0f;
};

// band-limited-ish carrier shapes for ring mod / FM
inline float carrier (int shape, double ph)
{
    const float s = std::sin ((float) (juce::MathConstants<double>::twoPi * ph));
    switch (shape)
    {
        case 1: return (float) (1.0 - 4.0 * std::abs (ph - 0.5));
        case 2: return fastTanh (4.0f * s);
        default: return s;
    }
}

// ------------------------------------------------------------------ Ring Mod
// Freq, Ratio, Shape, Drive, LFO Rate, LFO Depth, Stereo, Tone | Free / Track
class RingMod : public FxImpl
{
public:
    void prepare (double s) override { pt.prepare (s); reset(); }
    void reset() override { pt.reset(); phL = phR = 0.0; lfo.reset(); tl.reset(); tr.reset(); tracked = 220.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int shape = ci (c, 2);
        const float drive = 1.0f + c.p[3] * 6.0f;
        tl.setCutoff (c.p[7], sr); tr.setCutoff (c.p[7], sr);
        const double offR = c.p[6] * 0.25;
        float f = c.p[0];
        for (int i = 0; i < n; ++i)
        {
            if (c.mode == 1)
            {
                pt.push (0.5f * (L[i] + R[i]));
                if (pt.hz() > 0.0f) tracked += (pt.hz() - tracked) * 0.002f;
                f = tracked * c.p[1];
            }
            else f = c.p[0];
            f *= std::pow (2.0f, c.p[5] * lfo.valueAt (0, 0.0));
            lfo.advance (c.p[4] / sr);
            phL += f / sr; phL -= std::floor (phL);
            const float cl = carrier (shape, phL), cr = carrier (shape, phL + offR - std::floor (phL + offR));
            L[i] = tl.lp (fastTanh (L[i] * drive) / std::sqrt (drive) * cl);
            R[i] = tr.lp (fastTanh (R[i] * drive) / std::sqrt (drive) * cr);
        }
        taps->set (1, f); taps->set (0, (float) phL);
    }
private:
    PitchTracker pt;
    double phL = 0.0, phR = 0.0;
    Lfo lfo;
    OnePole tl, tr;
    float tracked = 220.0f;
};

// ------------------------------------------------------------------ FM
// Freq, Ratio, Amount, Feedback, Shape, Tone, Metal | Fixed / Track
// Audio-rate delay modulation = phase modulation of the input. Index reaches
// 20 radians; Metal adds a second operator at an inharmonic (sqrt 2) ratio.
class FmFx : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 0.15f); dr.prepare (s, 0.15f); pt.prepare (s); reset(); }
    void reset() override { dl.reset(); dr.reset(); pt.reset(); ph = ph2 = 0.0; last = 0.0f; tl.reset(); tr.reset(); tracked = 220.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int shape = ci (c, 4);
        tl.setCutoff (c.p[5], sr); tr.setCutoff (c.p[5], sr);
        const float metal = c.p[6];
        const float index = c.p[2] * c.p[2] * 20.0f;
        float f = c.p[0];
        for (int i = 0; i < n; ++i)
        {
            if (c.mode == 1)
            {
                pt.push (0.5f * (L[i] + R[i]));
                if (pt.hz() > 0.0f) tracked += (pt.hz() - tracked) * 0.002f;
                f = tracked * c.p[1];
            }
            f = juce::jmax (20.0f, f);
            const float A = juce::jmin (0.06f * (float) sr, index * (float) sr / (kTwoPi * f));
            ph += f / sr; ph -= std::floor (ph);
            ph2 += f * 1.41421356 / sr; ph2 -= std::floor (ph2);
            double pm = ph + c.p[3] * 0.25 * last;
            pm -= std::floor (pm);
            const float o1 = carrier (shape, pm);
            const float o2 = std::sin ((float) (ph2 * juce::MathConstants<double>::twoPi));
            last = o1;
            const float o = o1 * (1.0f - 0.5f * metal) + o2 * metal * 0.9f;
            dl.write (L[i]); dr.write (R[i]);
            L[i] = tl.lp (dl.readCubic (A * (1.5f + o) + 3.0f));
            R[i] = tr.lp (dr.readCubic (A * (1.5f + o) + 3.0f));
        }
        taps->set (1, f); taps->set (0, (float) ph);
    }
private:
    DelayLine dl, dr;
    PitchTracker pt;
    double ph = 0.0, ph2 = 0.0;
    float last = 0.0f, tracked = 220.0f;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Vibrato
// Rate, Depth (cents), Shape, Stereo, Tone, Morph, Freeze, Sync, Div
class Vibrato : public LfoFx
{
public:
    void prepare (double s) override { dl.prepare (s, 0.6f); dr.prepare (s, 0.6f); reset(); }
    void reset() override { dl.reset(); dr.reset(); lfo.reset(); tl.reset(); tr.reset(); ml = mr = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double inc = lfoInc (c, 7, 0, 8, 6);
        const float rate = juce::jmax (0.05f, (float) (inc * sr)) ;
        const float ratio = std::pow (2.0f, c.p[1] / 1200.0f) - 1.0f;
        // depth defined for a sine: peak slope 2 pi f A = ratio
        const float A = juce::jmin (0.25f * (float) sr, ratio * (float) sr / (kTwoPi * (on (c, 6) ? 1.0f : rate)));
        const double off = c.p[3] / 360.0;
        tl.setCutoff (c.p[4], sr); tr.setCutoff (c.p[4], sr);
        const float sm = msCoef (1.5f, sr);   // rounds off steps in square / S&H shapes
        for (int i = 0; i < n; ++i)
        {
            dl.write (L[i]); dr.write (R[i]);
            ml += (shapeValue (c, 2, 5) - ml) * sm;
            mr += (shapeValue (c, 2, 5, off) - mr) * sm;
            lfo.advance (inc);
            L[i] = tl.lp (dl.readCubic (A * (1.0f + ml) + 3.0f));
            R[i] = tr.lp (dr.readCubic (A * (1.0f + mr) + 3.0f));
        }
        publishPhase();
    }
private:
    DelayLine dl, dr;
    OnePole tl, tr;
    float ml = 0.0f, mr = 0.0f;
};

// ------------------------------------------------------------------ Doppler
// Speed, Distance, Width, Air | Pass / Orbit
class Doppler : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 1.4f); dr.prepare (s, 1.4f); reset(); }
    void reset() override { dl.reset(); dr.reset(); t = 0.0; al.reset(); ar.reset(); }
    // source position relative to the listener (metres)
    static void position (double t, float speed, float dist, int mode, float& x, float& y)
    {
        if (mode == 0)
        {
            const float half = juce::jmax (20.0f, speed * 2.5f);
            const double period = 2.0 * half / speed;
            const double u = t / period - std::floor (t / period);
            x = (float) (-half + 2.0 * half * u); y = dist;
        }
        else
        {
            const float R = juce::jmax (2.0f, dist * 1.5f);
            const double ang = t * speed / R;
            x = R * (float) std::cos (ang) + R * 0.85f; y = R * (float) std::sin (ang);
        }
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float speed = c.p[0], dist = c.p[1], width = c.p[2], air = c.p[3];
        float x = 0, y = 0, r = 1;
        for (int i = 0; i < n; ++i)
        {
            position (t, speed, dist, c.mode, x, y);
            t += 1.0 / sr;
            r = juce::jmax (0.5f, std::sqrt (x * x + y * y));
            const float d = r / 343.0f * (float) sr + 2.0f;
            dl.write (L[i]); dr.write (R[i]);
            const float g = juce::jmin (1.5f, dist / r);
            float gl, gr;
            panGains (juce::jlimit (-1.0f, 1.0f, x / r) * width, gl, gr);
            const float fc = juce::jlimit (600.0f, 20000.0f, 20000.0f * std::exp (-r * air * 0.025f));
            al.setCutoff (fc, sr); ar.setCutoff (fc, sr);
            L[i] = al.lp (dl.readCubic (d)) * g * gl;
            R[i] = ar.lp (dr.readCubic (d)) * g * gr;
        }
        const float span = c.mode == 0 ? juce::jmax (20.0f, speed * 2.5f) : juce::jmax (2.0f, dist * 1.5f) * 2.0f;
        taps->set (0, x / span); taps->set (1, y / span); taps->set (2, r);
    }
private:
    DelayLine dl, dr;
    double t = 0.0;
    OnePole al, ar;
};

// ------------------------------------------------------------------ Wobble
// Sync, Rate, Div, Depth, Cutoff, Reso, Shape, Drive, Morph, Freeze, Type (LP / BP / HP)
class Wobble : public LfoFx
{
public:
    void prepare (double s) override { for (auto& f : f1) f.prepare (s); for (auto& f : f2) f.prepare (s); reset(); }
    void reset() override { lfo.reset(); for (auto& f : f1) f.reset(); for (auto& f : f2) f.reset(); sm = 0.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const double inc = lfoInc (c, 0, 1, 2, 9);
        const float depth = c.p[3], base = c.p[4], res = c.p[5];
        const float drive = 1.0f + c.p[7] * 6.0f;
        const int type = juce::jlimit (0, 2, ci (c, 10));
        static constexpr int svfMode[3] = { 0, 2, 1 };
        const float smooth = msCoef (3.0f, sr);
        const float comp = 1.0f / (1.0f + res * 0.8f);
        float fc = base;
        for (int i = 0; i < n; ++i)
        {
            sm += (0.5f + 0.5f * shapeValue (c, 6, 8) - sm) * smooth;
            lfo.advance (inc);
            fc = juce::jmin (18000.0f, base * std::pow (2.0f, depth * 7.0f * sm));
            auto chan = [&] (Svf& a, Svf& b, float x)
            {
                x = fastTanh (x * drive) / std::sqrt (drive);
                return b.processNLMode (a.processNLMode (x, fc, res, svfMode[type]), fc, 0.0f, svfMode[type]) * comp;
            };
            L[i] = chan (f1[0], f2[0], L[i]); R[i] = chan (f1[1], f2[1], R[i]);
        }
        publishPhase(); taps->set (1, fc);
    }
private:
    Svf f1[2], f2[2];
    float sm = 0.0f;
};

// ------------------------------------------------------------------ Formant shifter (PSOLA)
// Formant, Pitch, Robot Hz, Tone, Out | Natural / Robot
class FormantShifter : public FxImpl
{
public:
    void prepare (double s) override { pt.prepare (s); pl.prepare (s); pr.prepare (s); reset(); }
    void reset() override { pt.reset(); pl.reset(); pr.reset(); tl.reset(); tr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float formant = semisToRatio (c.p[0]), pitch = semisToRatio (c.p[1]);
        const float robot = c.mode == 1 ? (float) sr / c.p[2] : 0.0f;
        tl.setCutoff (c.p[3], sr); tr.setCutoff (c.p[3], sr);
        const float out = dbToGain (c.p[4]);
        for (int i = 0; i < n; ++i)
        {
            pt.push (0.5f * (L[i] + R[i]));
            const float P = pt.confidence > 0.5f ? pt.periodSamples : 0.0f;
            L[i] = tl.lp (pl.process (L[i], P, pitch, formant, robot)) * out;
            R[i] = tr.lp (pr.process (R[i], P, pitch, formant, robot)) * out;
        }
        taps->set (0, pt.hz());
    }
private:
    PitchTracker pt;
    Psola pl, pr;
    OnePole tl, tr;
};

} // namespace

std::unique_ptr<FxImpl> makeModulationFx (const juce::String& n)
{
    if (n == "Autopan")         return std::make_unique<Autopan>();
    if (n == "Chorus")          return std::make_unique<Chorus>();
    if (n == "Flanger")         return std::make_unique<Flanger>();
    if (n == "Phaser")          return std::make_unique<Phaser>();
    if (n == "Wah Wah")         return std::make_unique<Wah>();
    if (n == "Tremolo")         return std::make_unique<Tremolo>();
    if (n == "Ring Mod")        return std::make_unique<RingMod>();
    if (n == "FM")              return std::make_unique<FmFx>();
    if (n == "Vibrato")         return std::make_unique<Vibrato>();
    if (n == "Doppler")         return std::make_unique<Doppler>();
    if (n == "Wobble")          return std::make_unique<Wobble>();
    if (n == "Formant Shifter") return std::make_unique<FormantShifter>();
    return nullptr;
}

} // namespace efx
