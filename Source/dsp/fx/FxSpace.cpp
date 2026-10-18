#include "FxCommon.h"

// Space: reverbs and delays. All are 100% wet; the slot's Mix blends the dry.
// Meter conventions (VizTaps::m):
//   delays: m[0] delay time L (ms), m[1] delay time R (ms), m[2] feedback
//   reverbs: m[0] decay (s), m[1] predelay (ms)
//   Granular Delay: m[0..23] active grain positions (0..1 of the window, <0 idle)

namespace efx
{
namespace
{

// predelay + lowcut + width wrapper shared by the reverbs
struct VerbIO
{
    DelayLine pl, pr;
    Biquad hl, hr;
    float lastCut = -1.0f;
    void prepare (double s) { pl.prepare (s, 0.25f); pr.prepare (s, 0.25f); }
    void reset() { pl.reset(); pr.reset(); hl.reset(); hr.reset(); lastCut = -1.0f; }
    void setLowcut (float hz, double sr) { if (std::abs (hz - lastCut) > 0.01f) { hl.setHighPass (hz, 0.707f, sr); hr.copyCoeffs (hl); lastCut = hz; } }
    // returns the predelayed, low-cut input
    inline void in (float l, float r, float predelaySamples, float& ol, float& orr)
    {
        pl.write (l); pr.write (r);
        ol = predelaySamples >= 1.0f ? pl.read (predelaySamples) : l;
        orr = predelaySamples >= 1.0f ? pr.read (predelaySamples) : r;
        if (lastCut > 21.0f) { ol = hl.process (ol); orr = hr.process (orr); }
    }
};

// ------------------------------------------------------------------ Early reflections
// Size, Predelay, Damp, Taps, Width, Lowcut | Room / Hall / Slap
class EarlyReflections : public FxImpl
{
public:
    static constexpr int kTaps = 16;
    void prepare (double s) override
    {
        dl.prepare (s, 0.5f); dr.prepare (s, 0.5f);
        Rng r; r.seed (2024);
        for (int m = 0; m < 3; ++m)
            for (int k = 0; k < kTaps; ++k)
            {
                const float base = m == 0 ? 2.5f : (m == 1 ? 7.0f : 0.0f);
                ms[m][k] = m == 2 ? 55.0f + 45.0f * (float) (k % 4) + 7.0f * r.uni()
                                  : base * std::pow ((float) (k + 1), 0.95f) * (0.85f + 0.3f * r.uni());
                gain[m][k] = (m == 2 ? 0.8f * std::pow (0.6f, (float) (k % 4)) : 0.9f * std::exp (-0.13f * (float) k))
                             * (r.uni() < 0.25f ? -1.0f : 1.0f);
                pan[m][k] = ((k & 1) ? 1.0f : -1.0f) * (0.3f + 0.7f * r.uni());
            }
        reset();
    }
    void reset() override { dl.reset(); dr.reset(); dampL.reset(); dampR.reset(); hl.reset(); hr.reset(); lastCut = -1.0f; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const int m = juce::jlimit (0, 2, c.mode);
        const float scale = 0.3f + 1.4f * c.p[0];
        const float pre = c.p[1] * 0.001f * (float) sr;
        const int nt = juce::jlimit (4, kTaps, ci (c, 3));
        dampL.setCutoff (c.p[2], sr); dampR.setCutoff (c.p[2], sr);
        if (std::abs (c.p[5] - lastCut) > 0.01f) { hl.setHighPass (c.p[5], 0.707f, sr); hr.copyCoeffs (hl); lastCut = c.p[5]; }
        const float norm = 1.6f / std::sqrt ((float) nt);
        for (int i = 0; i < n; ++i)
        {
            float a = L[i], b = R[i];
            if (c.p[5] > 21.0f) { a = hl.process (a); b = hr.process (b); }
            dl.write (a); dr.write (b);
            float ol = 0.0f, orr = 0.0f;
            for (int k = 0; k < nt; ++k)
            {
                const float d = pre + ms[m][k] * scale * 0.001f * (float) sr + 1.0f;
                const float g = gain[m][k];
                const float p = pan[m][k];
                // taps alternate sides; the far side hears the near-side tap a touch later
                ol += dl.read (d * (p < 0 ? 1.0f : 1.04f)) * g * (1.0f - juce::jmax (0.0f, p) * 0.7f);
                orr += dr.read (d * (p > 0 ? 1.0f : 1.04f)) * g * (1.0f + juce::jmin (0.0f, p) * 0.7f);
            }
            ol = dampL.lp (ol * norm); orr = dampR.lp (orr * norm);
            applyWidth (ol, orr, c.p[4]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, scale); taps->set (1, c.p[1]);
    }
    // tap pattern for the display (ms, gain)
    float ms[3][kTaps] {}, gain[3][kTaps] {}, pan[3][kTaps] {};
private:
    DelayLine dl, dr;
    OnePole dampL, dampR;
    Biquad hl, hr;
    float lastCut = -1.0f;
};

// ------------------------------------------------------------------ Room
// Size, Decay, Predelay, Damp, Lowcut, Width, Early | Wood / Stone
class Room : public FxImpl
{
public:
    void prepare (double s) override { io.prepare (s); fdn.prepare ((float) s); er.sr = s; er.prepare (s); reset(); }
    void reset() override { io.reset(); fdn.reset(); er.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        io.setLowcut (c.p[4], sr);
        const bool stone = c.mode == 1;
        const float damp = c.p[3] * (stone ? 1.6f : 1.0f);
        const float size = 0.15f + 0.45f * c.p[0];
        const float early = c.p[6];
        // early reflections: reuse the ER effect's room pattern
        float erP[8] = { c.p[0], 0.0f, damp, 10.0f, 1.0f, 20.0f, 0.0f, 0.0f };
        FxContext ec = c; ec.p = erP; ec.mode = 0;
        std::array<float, GridEffect::kChunk> el {}, eR {};
        float pre[2][GridEffect::kChunk];
        for (int i = 0; i < n; ++i) io.in (L[i], R[i], c.p[2] * 0.001f * (float) sr, pre[0][i], pre[1][i]);
        std::copy (pre[0], pre[0] + n, el.begin()); std::copy (pre[1], pre[1] + n, eR.begin());
        er.process (el.data(), eR.data(), n, ec);
        for (int i = 0; i < n; ++i)
        {
            float wl, wr;
            fdn.process (pre[0][i], pre[1][i], c.p[1], size, damp, stone ? 0.75f : 0.6f, 0.15f, wl, wr);
            float ol = wl + el[(size_t) i] * early, orr = wr + eR[(size_t) i] * early;
            applyWidth (ol, orr, c.p[5]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, c.p[1]); taps->set (1, c.p[2]);
    }
private:
    VerbIO io;
    FdnReverb fdn;
    EarlyReflections er;
    VizTaps dummy;
public:
    Room() { er.taps = &dummy; }
};

// ------------------------------------------------------------------ Plate
// Decay, Predelay, Damp, Lowcut, Mod, Width, Diffusion | Bright / Dark
class Plate : public FxImpl
{
public:
    void prepare (double s) override { io.prepare (s); plate.prepare ((float) s); reset(); }
    void reset() override { io.reset(); plate.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        io.setLowcut (c.p[3], sr);
        const float damp = c.p[2] * (c.mode == 1 ? 0.45f : 1.0f);
        for (int i = 0; i < n; ++i)
        {
            float a, b;
            io.in (L[i], R[i], c.p[1] * 0.001f * (float) sr, a, b);
            float ol, orr;
            plate.process (0.5f * (a + b), c.p[0], damp, c.p[6], c.p[4], ol, orr);
            applyWidth (ol, orr, c.p[5]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, c.p[0]); taps->set (1, c.p[1]);
    }
private:
    VerbIO io;
    DattorroPlate plate;
};

// ------------------------------------------------------------------ Spring
// Decay, Tension, Tone, Drip, Lowcut, Width | 2 Springs / 3 Springs
class Spring : public FxImpl
{
public:
    static constexpr int kAp = 14;
    void prepare (double s) override { for (auto& d : dly) d.prepare (s, 0.1f); rng.seed (8); reset(); }
    void reset() override
    {
        for (auto& d : dly) d.reset();
        for (auto& ch : ap) for (auto& a : ch) a.reset();
        for (auto& t : tone) t.reset();
        for (auto& f : fb) f = 0.0f;
        hl.reset(); hr.reset(); lastCut = -1.0f; lfo.reset(); drip = dripTarget = 0.0f;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        if (std::abs (c.p[4] - lastCut) > 0.01f) { hl.setHighPass (c.p[4], 0.707f, sr); hr.copyCoeffs (hl); lastCut = c.p[4]; }
        const int springs = c.mode == 1 ? 3 : 2;
        const float tension = c.p[1];
        const float a = -(0.45f + 0.4f * tension);       // dispersion: the "boing" chirp
        static constexpr float lenMs[3] = { 37.0f, 43.0f, 51.0f };
        for (auto& t : tone) t.setCutoff (c.p[2], sr);
        const float dripAmt = c.p[3];
        const float dripGlide = msCoef (25.0f, sr);
        for (int i = 0; i < n; ++i)
        {
            // scaled so the tank's resonant build-up stays well inside the soft limiter
            const float in = 0.1f * 0.5f * (hl.process (L[i]) + hr.process (R[i]));
            lfo.advance (6.5 / sr);
            if (rng.uni() < 0.0004f) dripTarget = rng.uni();
            dripTarget *= 0.9997f;
            drip += (dripTarget - drip) * dripGlide;       // glide: no jumps in the delay time
            const float wob = dripAmt * (0.4f * lfo.valueAt (0, 0.0) + drip) * 0.0008f * (float) sr;
            float ol = 0.0f, orr = 0.0f;
            for (int s2 = 0; s2 < springs; ++s2)
            {
                const float len = lenMs[s2] * (1.25f - 0.5f * tension) * 0.001f * (float) sr;
                const float g = std::pow (10.0f, -3.0f * (len / (float) sr) / c.p[0]);
                float x = in + fb[s2] * g;
                for (int k = 0; k < kAp; ++k) x = ap[s2][k].process (x, a);
                dly[s2].write (tone[s2].lp (x));
                const float y = dly[s2].readCubic (len + wob * (1.0f + 0.3f * (float) s2));
                fb[s2] = 4.0f * fastTanh (y * 0.25f);        // safety limiter, only in the loop and transparent below ~1
                const float pan = springs == 3 ? (float) (s2 - 1) : (s2 == 0 ? -0.6f : 0.6f);
                ol += y * (1.0f - juce::jmax (0.0f, pan)); orr += y * (1.0f + juce::jmin (0.0f, pan));
            }
            ol *= 4.9f; orr *= 4.9f;
            applyWidth (ol, orr, c.p[5]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, c.p[0]);
    }
private:
    DelayLine dly[3];
    Allpass1 ap[3][kAp];
    OnePole tone[3];
    float fb[3] {};
    Biquad hl, hr;
    float lastCut = -1.0f, drip = 0.0f, dripTarget = 0.0f;
    Lfo lfo;
    Rng rng;
};

// ------------------------------------------------------------------ Shimmer
// Pitch, Shimmer, Decay, Predelay, Damp, Size, Width, Chord, Grain, Spray, Density, Lowcut
// The reverb tail is fed back through granular pitch voices (one or a chord of
// intervals above Pitch); Spray / Density / Grain shape the sparkle.
class Shimmer : public FxImpl
{
public:
    void prepare (double s) override { io.prepare (s); fdn.prepare ((float) s); gl.prepare (s, 101); gr.prepare (s, 202); reset(); }
    void reset() override { io.reset(); fdn.reset(); gl.reset(); gr.reset(); fl = fr = 0.0f; hpL.reset(); hpR.reset(); }
    static int chordRatios (int chord, float pitch, float* r)
    {
        static constexpr int iv[7][3] = { { 0, 99, 99 }, { 0, 7, 99 }, { 0, 12, 99 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 5, 7 }, { 0, -12, 99 } };
        int n = 0;
        for (int k = 0; k < 3; ++k)
            if (iv[juce::jlimit (0, 6, chord)][k] != 99) r[n++] = semisToRatio (pitch + (float) iv[juce::jlimit (0, 6, chord)][k]);
        return n;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        io.setLowcut (20.0f, sr);
        float ratios[3];
        const int nr = chordRatios (ci (c, 7), c.p[0], ratios);
        const float amt = c.p[1];
        hpL.setCutoff (c.p[11], sr); hpR.setCutoff (c.p[11], sr);
        const int grain = juce::jmax (64, (int) (c.p[8] * 0.001f * sr));
        const float rate = 6.0f + c.p[10] * 60.0f, spray = c.p[9];
        for (int i = 0; i < n; ++i)
        {
            float a, b;
            io.in (L[i], R[i], c.p[3] * 0.001f * (float) sr, a, b);
            float wl, wr;
            fdn.process (a + fl * amt * 0.7f, b + fr * amt * 0.7f, c.p[2], c.p[5], c.p[4], 0.7f, 0.35f, wl, wr);
            fl = loopSat (hpL.hp (gl.process (wl, ratios, nr, grain, rate, spray, sr)));
            fr = loopSat (hpR.hp (gr.process (wr, ratios, nr, grain, rate, spray, sr)));
            float ol = wl + fl * amt * 0.6f, orr = wr + fr * amt * 0.6f;
            applyWidth (ol, orr, c.p[6]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, c.p[2]); taps->set (1, c.p[3]);
    }
private:
    VerbIO io;
    FdnReverb fdn;
    GrainPitch gl, gr;
    OnePole hpL, hpR;
    float fl = 0.0f, fr = 0.0f;
};

// ------------------------------------------------------------------ Algorithmic (FDN hall)
// Size, Decay, Predelay, Damp, Diffusion, Mod, Width, Lowcut | Hall / Chamber / Cathedral
class AlgoVerb : public FxImpl
{
public:
    void prepare (double s) override { io.prepare (s); fdn.prepare ((float) s); reset(); }
    void reset() override { io.reset(); fdn.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        io.setLowcut (c.p[7], sr);
        static constexpr float sizeMul[3] = { 0.8f, 0.5f, 1.0f }, decayMul[3] = { 1.0f, 0.8f, 1.4f };
        const int m = juce::jlimit (0, 2, c.mode);
        const float size = clamp01 (c.p[0] * sizeMul[m] + (m == 2 ? 0.15f : 0.0f));
        const float diff = clamp01 (c.p[4] + (m == 1 ? 0.15f : 0.0f));
        for (int i = 0; i < n; ++i)
        {
            float a, b;
            io.in (L[i], R[i], c.p[2] * 0.001f * (float) sr, a, b);
            float ol, orr;
            fdn.process (a, b, c.p[1] * decayMul[m], size, c.p[3], diff, c.p[5], ol, orr);
            applyWidth (ol, orr, c.p[6]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, c.p[1] * decayMul[m]); taps->set (1, c.p[2]);
    }
private:
    VerbIO io;
    FdnReverb fdn;
};

// ------------------------------------------------------------------ Convolution
// Length, Predelay, Damp, Lowcut, Width, IR, Stretch, Highcut
// The built-in IRs are generated (filtered noise with type-specific build-up
// and early reflections); "User File" uses the slot's loaded audio file.
// IRs are (re)built on the message thread when the relevant controls change.
class ConvVerb : public FxImpl
{
public:
    static constexpr int kUser = 10;
    ConvVerb() : conv (juce::dsp::Convolution::NonUniform { 512 }) {}
    void prepare (double s) override
    {
        io.prepare (s);
        conv.prepare ({ s, (juce::uint32) GridEffect::kChunk, 2 });
        float defaults[kMaxParams];
        for (int k = 0; k < kMaxParams; ++k) defaults[k] = unmapParam (def->p[k], def->p[k].def);
        rebuild (defaults);
        reset();
    }
    void reset() override { io.reset(); conv.reset(); hcL.reset(); hcR.reset(); }

    struct Kind { float lenMul, attackMs, bright; int er; };   // er: 0 none, 1 hall, 2 room, 3 chamber, 4 cave, 5 cathedral
    static Kind kindFor (int ir)
    {
        static constexpr Kind k[10] = {
            { 1.0f, 35.0f, 1.0f, 1 }, { 1.6f, 60.0f, 0.9f, 1 }, { 0.45f, 4.0f, 1.0f, 2 }, { 0.25f, 2.0f, 1.1f, 2 },
            { 0.7f, 12.0f, 1.2f, 3 }, { 0.85f, 1.0f, 1.5f, 0 }, { 2.2f, 90.0f, 0.8f, 5 }, { 1.3f, 20.0f, 0.5f, 4 },
            { 0.6f, 1.0f, 1.3f, 0 }, { 1.0f, 1.0f, 1.0f, 0 } };
        return k[juce::jlimit (0, 9, ir)];
    }
    static juce::AudioBuffer<float> makeIR (int ir, float length, float damp, float stretch, double sr)
    {
        const auto k = kindFor (ir);
        const float rt = juce::jmax (0.1f, length * k.lenMul * stretch);
        const int len = juce::jlimit (64, (int) (8.5 * sr), (int) (rt * 1.1f * (float) sr));
        juce::AudioBuffer<float> buf (2, len);
        Rng rng; rng.seed (0xC0FFEEu + (juce::uint32) ir * 977u);
        const float dampEff = juce::jmin (20000.0f, damp * k.bright);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buf.getWritePointer (ch);
            OnePole lp; float z2 = 0.0f;
            for (int i = 0; i < len; ++i)
            {
                const float t = (float) i / (float) sr;
                const float fc = dampEff + (19000.0f - dampEff) * std::exp (-t * 4.0f / rt);
                if ((i & 15) == 0) lp.setCutoff (fc, sr);
                float env = std::exp (-6.9f * t / rt) * (1.0f - std::exp (-t * 1000.0f / (k.attackMs * stretch)));
                if (ir == 9) env = std::pow ((float) i / (float) len, 2.0f);           // reverse: swells into the dry
                float v = lp.lp (rng.bipolar());
                if (ir == 8) v *= 0.6f + 0.4f * std::sin (kTwoPi * 27.0f * t + (float) ch);   // spring: periodic flutter
                z2 += (v - z2) * 0.9f;
                d[i] = z2 * env;
            }
            if (k.er != 0)
            {
                static constexpr float erMs[5][6] = { { 13, 21, 29, 37, 48, 61 }, { 3, 5, 8, 11, 14, 19 }, { 6, 11, 17, 23, 31, 38 },
                                                      { 40, 95, 160, 230, 310, 420 }, { 25, 60, 110, 170, 240, 330 } };
                for (int e = 0; e < 6; ++e)
                {
                    const int at = (int) (erMs[k.er - 1][e] * stretch * (1.0f + 0.07f * (float) ch) * 0.001f * (float) sr);
                    const float lvl = k.er == 4 ? 0.7f - 0.08f * (float) e : 0.5f - 0.06f * (float) e;
                    if (at < len) d[at] += lvl * (rng.uni() < 0.3f ? -1.0f : 1.0f);
                }
            }
        }
        return buf;
    }
    static juce::AudioBuffer<float> userIR (const AudioAsset& a, float length, float damp, float stretch, double sr)
    {
        // resample (with stretch) to the host rate, fade out past Length, darken by Damp
        const double ratio = a.sampleRate / sr / stretch;
        const int srcLen = a.buf.getNumSamples();
        const int len = juce::jlimit (16, (int) (8.5 * sr), (int) ((double) srcLen / ratio));
        juce::AudioBuffer<float> out (2, len);
        const int fadeAt = (int) (length * (float) sr), fadeLen = (int) (0.25 * sr);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = a.buf.getReadPointer (juce::jmin (ch, a.buf.getNumChannels() - 1));
            float* d = out.getWritePointer (ch);
            OnePole lp;
            for (int i = 0; i < len; ++i)
            {
                const double sp = (double) i * ratio;
                const int i0 = juce::jmin (srcLen - 1, (int) sp);
                const float fr = (float) (sp - (double) i0);
                float v = src[i0] + (src[juce::jmin (srcLen - 1, i0 + 1)] - src[i0]) * fr;
                if (damp < 19000.0f)
                {
                    if ((i & 15) == 0) lp.setCutoff (damp + (19000.0f - damp) * std::exp (-(float) i / (float) sr * 4.0f / juce::jmax (0.1f, length)), sr);
                    v = lp.lp (v);
                }
                if (i > fadeAt) v *= juce::jmax (0.0f, 1.0f - (float) (i - fadeAt) / (float) fadeLen);
                d[i] = v;
            }
        }
        return out;
    }
    void rebuild (const float* p)
    {
        builtLen = p[0]; builtDamp = p[2]; builtIr = ci (p, 5); builtStretch = p[6];
        builtAsset = assetRef != nullptr ? assetRef->load() : nullptr;
        auto ir = (builtIr == kUser && builtAsset != nullptr) ? userIR (*builtAsset, p[0], p[2], p[6], sr)
                                                              : makeIR (builtIr == kUser ? 0 : builtIr, p[0], p[2], p[6], sr);
        conv.loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::yes,
                                  juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::yes);
    }
    void idle (const float* p, int) override
    {
        const auto* a = assetRef != nullptr ? assetRef->load() : nullptr;
        if (ci (p, 5) != builtIr || a != builtAsset || std::abs (p[0] - builtLen) > 0.02f * builtLen
            || std::abs (p[2] - builtDamp) > 0.03f * builtDamp || std::abs (p[6] - builtStretch) > 0.01f)
            rebuild (p);
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        io.setLowcut (c.p[3], sr);
        for (int i = 0; i < n; ++i) io.in (L[i], R[i], c.p[1] * 0.001f * (float) sr, L[i], R[i]);
        float* chans[2] = { L, R };
        juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
        conv.process (juce::dsp::ProcessContextReplacing<float> (block));
        hcL.setCutoff (c.p[7], sr); hcR.setCutoff (c.p[7], sr);
        const bool hc = c.p[7] < 19000.0f;
        for (int i = 0; i < n; ++i)
        {
            float l = L[i] * 3.5f, r = R[i] * 3.5f;
            if (hc) { l = hcL.lp (l); r = hcR.lp (r); }
            applyWidth (l, r, c.p[4]);
            L[i] = l; R[i] = r;
        }
        taps->set (0, c.p[0] * kindFor (juce::jmin (9, ci (c, 5))).lenMul * c.p[6]); taps->set (1, c.p[1]);
    }
private:
    VerbIO io;
    juce::dsp::Convolution conv;
    OnePole hcL, hcR;
    float builtLen = 0.0f, builtDamp = 0.0f, builtStretch = 1.0f;
    int builtIr = -1;
    const AudioAsset* builtAsset = nullptr;
};

// ------------------------------------------------------------------ delay helpers
inline float delayMs (const FxContext& c, const EffectDef& d, int iSync, int iTime, int iDiv)
{
    if (on (c, iSync)) return (float) (divBeatsOf (d, c.p, iDiv) * 60000.0 / juce::jmax (20.0, c.bpm));
    return c.p[iTime];
}

struct LoopFilter
{
    OnePole lp; Biquad hp; float lastHp = -1.0f;
    void reset() { lp.reset(); hp.reset(); lastHp = -1.0f; }
    void set (float tone, float lowcut, double sr)
    {
        lp.setCutoff (tone, sr);
        if (std::abs (lowcut - lastHp) > 0.01f) { hp.setHighPass (lowcut, 0.707f, sr); lastHp = lowcut; }
    }
    inline float process (float x) { x = lp.lp (x); return lastHp > 21.0f ? hp.process (x) : x; }
};

// ------------------------------------------------------------------ Simple Delay
// Sync, Time, Div, Feedback, Tone, Lowcut
class SimpleDelay : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 2.6f); dr.prepare (s, 2.6f); reset(); }
    void reset() override { dl.reset(); dr.reset(); fl.reset(); fr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float ms = juce::jmin (2500.0f, delayMs (c, *def, 0, 1, 2));
        const float d = ms * 0.001f * (float) sr;
        const float fb = c.p[3];
        fl.set (c.p[4], c.p[5], sr); fr.set (c.p[4], c.p[5], sr);
        for (int i = 0; i < n; ++i)
        {
            const float yl = dl.readCubic (d), yr = dr.readCubic (d);
            const float wl = fl.process (yl), wr = fr.process (yr);
            dl.write (L[i] + loopSat (wl * fb)); dr.write (R[i] + loopSat (wr * fb));
            L[i] = wl; R[i] = wr;
        }
        taps->set (0, ms); taps->set (1, ms); taps->set (2, fb);
    }
private:
    DelayLine dl, dr;
    LoopFilter fl, fr;
};

// ------------------------------------------------------------------ Precision Delay
// Sync, Time, Div, Feedback, Offset, Duck, Tone, Lowcut, Repeats, Decay, Spacing, Ping, Width | Repeats / Feedback
// Repeats mode is a multi-tap delay: exactly N echoes, each Decay dB quieter,
// spaced evenly (or accelerating / slowing down with Spacing). Feedback mode is
// a classic feedback delay.
class PrecisionDelay : public FxImpl
{
public:
    static constexpr int kMaxTaps = 16;
    static constexpr float kMaxSec = 20.0f;
    void prepare (double s) override { dl.prepare (s, kMaxSec + 0.1f); dr.prepare (s, kMaxSec + 0.1f); reset(); }
    void reset() override { dl.reset(); dr.reset(); fl.reset(); fr.reset(); env.reset(); fbl = fbr = 0.0f; }
    // tap times (ms, relative to the first echo time) for the visual and the DSP
    static int tapTimes (float t, int repeats, float spacing, float* out)
    {
        const float r = std::pow (2.0f, spacing * 0.6f);
        float acc = 0.0f, step = t;
        int n = 0;
        for (int k = 0; k < repeats; ++k)
        {
            acc += step;
            if (acc > kMaxSec * 1000.0f) break;
            out[n++] = acc;
            step *= r;
        }
        return n;
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float ms = juce::jmin (kMaxSec * 1000.0f, delayMs (c, *def, 0, 1, 2));
        const float off = c.p[4], duck = c.p[5];
        fl.set (c.p[6], c.p[7], sr); fr.set (c.p[6], c.p[7], sr);
        env.set (5.0f, 250.0f, sr);
        const bool repeatsMode = c.mode == 0, ping = on (c, 11);
        const float width = c.p[12];
        float times[kMaxTaps], gains[kMaxTaps];
        const int nTaps = repeatsMode ? tapTimes (ms, juce::jlimit (1, kMaxTaps, ci (c, 8)), c.p[10], times) : 0;
        for (int k = 0; k < nTaps; ++k) gains[k] = dbToGain (c.p[9] * (float) k);
        const float fb = c.p[3];
        const float dL = ms * 0.001f * (float) sr, dR = juce::jmax (0.1f, ms + off) * 0.001f * (float) sr;
        for (int i = 0; i < n; ++i)
        {
            float yl = 0.0f, yr = 0.0f;
            if (repeatsMode)
            {
                const float in = 0.5f * (L[i] + R[i]);
                dl.write (ping ? in : L[i]); dr.write (ping ? in : R[i]);
                for (int k = 0; k < nTaps; ++k)
                {
                    const float tl = times[k] * 0.001f * (float) sr, tr = (times[k] + off) * 0.001f * (float) sr;
                    if (ping) { if ((k & 1) == 0) yl += dl.read (tl) * gains[k]; else yr += dr.read (juce::jmax (1.0f, tr)) * gains[k]; }
                    else { yl += dl.read (tl) * gains[k]; yr += dr.read (juce::jmax (1.0f, tr)) * gains[k]; }
                }
            }
            else
            {
                yl = dl.read (dL); yr = dr.read (dR);
                if (ping) { dl.write (0.5f * (L[i] + R[i]) + yr * fb); dr.write (yl * fb); }
                else { dl.write (L[i] + yl * fb); dr.write (R[i] + yr * fb); }
            }
            yl = fl.process (yl); yr = fr.process (yr);
            const float e = env.process (juce::jmax (std::abs (L[i]), std::abs (R[i])));
            const float g = duck > 0.01f ? dbToGain (-duck * clamp01 ((gainToDb (e) + 40.0f) / 30.0f)) : 1.0f;
            float ol = yl * g, orr = yr * g;
            applyWidth (ol, orr, width);
            L[i] = juce::jlimit (-4.0f, 4.0f, ol); R[i] = juce::jlimit (-4.0f, 4.0f, orr);
        }
        taps->set (0, ms); taps->set (1, juce::jmax (0.1f, ms + off)); taps->set (2, repeatsMode ? dbToGain (c.p[9]) : fb);
        taps->set (3, repeatsMode ? (float) nTaps : 0.0f); taps->set (4, c.p[10]); taps->set (5, ping ? 1.0f : 0.0f);
    }
private:
    DelayLine dl, dr;
    LoopFilter fl, fr;
    EnvFollower env;
    float fbl = 0.0f, fbr = 0.0f;
};

// ------------------------------------------------------------------ Ping-Pong
// Sync, Time, Div, Feedback, Width, Tone, Lowcut
class PingPong : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 2.6f); dr.prepare (s, 2.6f); reset(); }
    void reset() override { dl.reset(); dr.reset(); fl.reset(); fr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float ms = juce::jmin (2500.0f, delayMs (c, *def, 0, 1, 2));
        const float d = ms * 0.001f * (float) sr;
        const float fb = c.p[3];
        fl.set (c.p[5], c.p[6], sr); fr.set (c.p[5], c.p[6], sr);
        for (int i = 0; i < n; ++i)
        {
            const float yl = fl.process (dl.readCubic (d)), yr = fr.process (dr.readCubic (d));
            dl.write (0.5f * (L[i] + R[i]) + loopSat (yr * fb));   // input enters left, bounces right
            dr.write (loopSat (yl * fb));
            float ol = yl, orr = yr;
            applyWidth (ol, orr, c.p[4]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, ms); taps->set (1, ms * 2.0f); taps->set (2, fb);
    }
private:
    DelayLine dl, dr;
    LoopFilter fl, fr;
};

// ------------------------------------------------------------------ Spectral delay
// Time, Spread, Feedback, Diffuse, Tone, Lowcut, Bands, Freeze, Width
// Shape data, per band b at b*4: time multiplier, feedback (x master), level dB, pan
class SpectralDelay : public FxImpl
{
public:
    static constexpr int NB = Shape::kSdBands;
    bool usesData() const override { return true; }
    void defaultData (float* d) const override
    {
        for (int b = 0; b < NB; ++b) { d[b * 4] = 1.0f; d[b * 4 + 1] = 1.0f; d[b * 4 + 2] = 0.0f; d[b * 4 + 3] = (b & 1) ? 0.5f : -0.5f; }
    }
    void prepare (double s) override
    {
        configure (8);
        for (auto& d : dl) d.prepare (s, 4.1f);
        for (auto& d : dr) d.prepare (s, 4.1f);
        for (auto& a : apl) a.prepare (s, 0.02f);
        for (auto& a : apr) a.prepare (s, 0.02f);
        reset();
    }
    void configure (int nb) { bands = nb; spL.prepare (sr, nb, 200.0f, 6000.0f); spR.prepare (sr, nb, 200.0f, 6000.0f); }
    void reset() override
    {
        spL.reset(); spR.reset(); for (auto& d : dl) d.reset(); for (auto& d : dr) d.reset();
        for (auto& a : apl) a.reset(); for (auto& a : apr) a.reset(); fl.reset(); fr.reset();
    }
    static float bandTime (float t, float spread, int b, int nb, const float* d)
    {
        return t * juce::jlimit (0.05f, 8.0f, d[b * 4]) * std::pow (2.0f, spread * 2.0f * ((float) b / (float) juce::jmax (1, nb - 1) - 0.5f));
    }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        static constexpr int kCounts[3] = { 4, 6, 8 };
        const int nb = kCounts[juce::jlimit (0, 2, ci (c, 6))];
        if (nb != bands) { configure (nb); reset(); }
        const bool freeze = on (c, 7);
        float d[NB], fb[NB], gain[NB], gl[NB], gr[NB];
        for (int b = 0; b < nb; ++b)
        {
            d[b] = juce::jmin (4000.0f, bandTime (c.p[0], c.p[1], b, nb, c.data)) * 0.001f * (float) sr;
            fb[b] = freeze ? 1.0f : juce::jmin (0.97f, c.p[2] * juce::jlimit (0.0f, 2.0f, c.data[b * 4 + 1]));
            gain[b] = dbToGain (juce::jlimit (-60.0f, 12.0f, c.data[b * 4 + 2]));
            panGains (juce::jlimit (-1.0f, 1.0f, c.data[b * 4 + 3]) * c.p[8], gl[b], gr[b]);
        }
        const float diff = c.p[3] * 0.6f;
        fl.set (c.p[4], c.p[5], sr); fr.set (c.p[4], c.p[5], sr);
        for (int i = 0; i < n; ++i)
        {
            float bl[NB], br[NB];
            spL.split (freeze ? 0.0f : L[i], bl); spR.split (freeze ? 0.0f : R[i], br);
            float ol = 0.0f, orr = 0.0f;
            for (int b = 0; b < nb; ++b)
            {
                float yl = dl[b].read (d[b]), yr = dr[b].read (d[b]);
                if (diff > 0.01f) { yl = apl[b].process (yl, (5.0f + 1.7f * b) * 0.001f * (float) sr, diff); yr = apr[b].process (yr, (5.6f + 1.9f * b) * 0.001f * (float) sr, diff); }
                dl[b].write (bl[b] + loopSat (yl * fb[b])); dr[b].write (br[b] + loopSat (yr * fb[b]));
                const float m = 0.5f * (yl + yr) * gain[b], sd = 0.5f * (yl - yr) * gain[b];
                ol += m * gl[b] + sd; orr += m * gr[b] - sd;
            }
            L[i] = fl.process (ol); R[i] = fr.process (orr);
        }
        for (int b = 0; b < NB; ++b) taps->set (b, b < nb ? d[b] * 1000.0f / (float) sr : -1.0f);
        taps->set (VizTaps::kMeters - 1, (float) nb);
    }
private:
    BandSplitter<NB> spL, spR;
    int bands = NB;
    DelayLine dl[NB], dr[NB];
    AllpassDelay apl[NB], apr[NB];
    LoopFilter fl, fr;
};

// ------------------------------------------------------------------ Echo (tape / BBD)
// Sync, Time, Div, Feedback, Tone, Wow, Flutter, Drive | Tape / BBD
class Echo : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 2.7f); dr.prepare (s, 2.7f); rng.seed (3); reset(); }
    void reset() override { dl.reset(); dr.reset(); fl.reset(); fr.reset(); wow.reset(); flut.reset(); drift = 0.0f; hl.reset(); hr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float ms = juce::jmin (2500.0f, delayMs (c, *def, 0, 1, 2));
        const float base = ms * 0.001f * (float) sr;
        const float fb = c.p[3];
        const bool bbd = c.mode == 1;
        const float tone = c.p[4] * (bbd ? 0.6f : 1.0f);
        fl.setCutoff (tone, sr); fr.setCutoff (tone * 0.97f, sr);
        hl.setCutoff (90.0f, sr); hr.setCutoff (90.0f, sr);
        const float wowD = c.p[5] * 0.0035f * (float) sr, flD = c.p[6] * 0.0003f * (float) sr;
        const float drive = 1.0f + c.p[7] * 4.0f;
        if (rng.uni() < 0.01f) driftT = rng.bipolar();
        for (int i = 0; i < n; ++i)
        {
            drift += (driftT - drift) * 0.00002f;
            const float m = wowD * (wow.valueAt (0, 0.0) + 0.5f * drift) + flD * flut.valueAt (0, 0.0);
            wow.advance (0.6 / sr); flut.advance (6.3 / sr);
            const float yl = dl.readCubic (base + m + 4.0f), yr = dr.readCubic (base * 1.003f + m + 4.0f);
            float wl = hl.hp (fl.lp (yl)), wr = hr.hp (fr.lp (yr));
            if (bbd) { wl += rng.bipolar() * 0.0004f; wr += rng.bipolar() * 0.0004f; }
            dl.write (fastTanh ((L[i] + wl * fb) * drive) / drive);
            dr.write (fastTanh ((R[i] + wr * fb) * drive) / drive);
            L[i] = wl; R[i] = wr;
        }
        taps->set (0, ms); taps->set (1, ms); taps->set (2, fb);
    }
private:
    DelayLine dl, dr;
    OnePole fl, fr, hl, hr;
    Lfo wow, flut;
    Rng rng;
    float drift = 0.0f, driftT = 0.0f;
};

// ------------------------------------------------------------------ Warp verb/delay
// Warp, Time, Feedback, Decay, Mod, Tone, Freeze, Width
class Warp : public FxImpl
{
public:
    void prepare (double s) override { dl.prepare (s, 1.7f); dr.prepare (s, 1.7f); fdn.prepare ((float) s); reset(); }
    void reset() override { dl.reset(); dr.reset(); fdn.reset(); lfo.reset(); tl.reset(); tr.reset(); }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float warp = c.p[0];
        const float d = c.p[1] * 0.001f * (float) sr;
        const bool freeze = on (c, 6);
        const float fb = freeze ? 1.0f : c.p[2];
        const float decay = freeze ? 60.0f : c.p[3];
        const float mod = c.p[4];
        tl.setCutoff (c.p[5], sr); tr.setCutoff (c.p[5], sr);
        for (int i = 0; i < n; ++i)
        {
            const float m = mod * 0.004f * (float) sr * lfo.valueAt (0, 0.0);
            lfo.advance (0.35 / sr);
            const float inL = freeze ? 0.0f : L[i], inR = freeze ? 0.0f : R[i];
            const float yl = tl.lp (dl.readCubic (d + m + 4.0f)), yr = tr.lp (dr.readCubic (d - m + 4.0f));
            dl.write (inL + loopSat (yr * fb)); dr.write (inR + loopSat (yl * fb));
            float vl, vr;
            fdn.process ((inL + yl) * warp, (inR + yr) * warp, decay, 0.4f + 0.5f * warp, c.p[5], warp, mod, vl, vr);
            float ol = lerp (yl, vl, warp), orr = lerp (yr, vr, warp);
            applyWidth (ol, orr, c.p[7]);
            L[i] = ol; R[i] = orr;
        }
        taps->set (0, c.p[1]); taps->set (1, c.p[1]); taps->set (2, fb);
    }
private:
    DelayLine dl, dr;
    FdnReverb fdn;
    Lfo lfo;
    OnePole tl, tr;
};

// ------------------------------------------------------------------ Granular delay
// Time, Grain, Density, Spray, Pitch, Detune, Feedback, Reverse, Window, Quantize, Width, Tone, Freeze
class GranularDelay : public FxImpl
{
public:
    static constexpr int kG = 24;
    struct Grain { double pos = 0.0, inc = 1.0; int len = 0, age = 0; float gl = 1.0f, gr = 1.0f; bool active = false; };
    void prepare (double s) override { hist.prepare (s, 3.5f); rng.seed (11); reset(); }
    void reset() override { hist.reset(); for (auto& g : grains) g.active = false; spawn = 0.0; fbl = fbr = 0.0f; tl.reset(); tr.reset(); frozenAt = -1; }
    void process (float* L, float* R, int n, const FxContext& c) override
    {
        const float timeS = c.p[0] * 0.001f * (float) sr;
        const int glen = juce::jmax (16, (int) (c.p[1] * 0.001f * sr));
        const float rate = 2.0f + c.p[2] * 46.0f;      // grains per second
        const float spray = c.p[3], fb = c.p[6], rev = c.p[7], width = c.p[10];
        const int window = ci (c, 8), quant = ci (c, 9);
        const bool freeze = on (c, 12);
        tl.setCutoff (c.p[11], sr); tr.setCutoff (c.p[11], sr);
        for (int i = 0; i < n; ++i)
        {
            if (! freeze) { hist.write (L[i] + loopSat (fbl * fb), R[i] + loopSat (fbr * fb)); frozenAt = -1; }
            else if (frozenAt < 0) frozenAt = hist.w;
            const long long now = freeze ? frozenAt : hist.w;
            spawn += rate / sr;
            if (spawn >= 1.0)
            {
                spawn -= 1.0;
                for (auto& g : grains)
                    if (! g.active)
                    {
                        const float semis = c.p[4] + quantizedDetune (rng, c.p[5] * 12.0f, quant);
                        const double ratio = semisToRatio (semis);
                        const bool back = rng.uni() < rev;
                        g.inc = back ? -ratio : ratio;
                        const double reach = (double) glen * juce::jmax (1.0, ratio) + 4.0;
                        const double start = (double) now - timeS - spray * rng.uni() * 1.0 * sr - reach;
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
            fbl = tl.lp (ol * norm); fbr = tr.lp (orr * norm);
            L[i] = fbl; R[i] = fbr;
        }
        int k = 0;
        for (auto& g : grains) taps->set (k++, g.active ? (float) g.age / (float) juce::jmax (1, g.len) : -1.0f);
    }
private:
    History hist;
    Grain grains[kG];
    Rng rng;
    double spawn = 0.0;
    float fbl = 0.0f, fbr = 0.0f;
    OnePole tl, tr;
    long long frozenAt = -1;
};

} // namespace

std::unique_ptr<FxImpl> makeSpaceFx (const juce::String& n)
{
    if (n == "Early Reflections") return std::make_unique<EarlyReflections>();
    if (n == "Room Reverb")       return std::make_unique<Room>();
    if (n == "Plate Reverb")      return std::make_unique<Plate>();
    if (n == "Spring Reverb")     return std::make_unique<Spring>();
    if (n == "Shimmer Reverb")    return std::make_unique<Shimmer>();
    if (n == "Algorithmic Verb")  return std::make_unique<AlgoVerb>();
    if (n == "Convolution Verb")  return std::make_unique<ConvVerb>();
    if (n == "Simple Delay")      return std::make_unique<SimpleDelay>();
    if (n == "Precision Delay")   return std::make_unique<PrecisionDelay>();
    if (n == "Ping-Pong Delay")   return std::make_unique<PingPong>();
    if (n == "Spectral Delay")    return std::make_unique<SpectralDelay>();
    if (n == "Echo")              return std::make_unique<Echo>();
    if (n == "Warp Verb/Delay")   return std::make_unique<Warp>();
    if (n == "Granular Delay")    return std::make_unique<GranularDelay>();
    return nullptr;
}

} // namespace efx
