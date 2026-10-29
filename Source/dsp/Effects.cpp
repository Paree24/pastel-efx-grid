#include "Effects.h"
#include <cmath>

namespace efx
{

// ============================================================
// Wavetable
// ============================================================
float Wavetable::sample (float frame01, float phase01) const
{
    if (numFrames <= 0) return 0.0f;
    const float fp = clamp01 (frame01) * (float) (numFrames - 1);
    const int f0 = juce::jlimit (0, numFrames - 1, (int) fp);
    const int f1 = juce::jmin (numFrames - 1, f0 + 1);
    const float ff = fp - (float) f0;
    phase01 -= std::floor (phase01);
    const float pp = phase01 * (float) kFrameLen;
    const int i0 = (int) pp & (kFrameLen - 1);
    const int i1 = (i0 + 1) & (kFrameLen - 1);
    const float pf = pp - std::floor (pp);
    const float* a = frame (f0);
    const float* b = frame (f1);
    const float va = a[i0] + (a[i1] - a[i0]) * pf;
    const float vb = b[i0] + (b[i1] - b[i0]) * pf;
    return va + (vb - va) * ff;
}

float Wavetable::harmonic (float frame01, int h) const
{
    if (numFrames <= 0 || h < 0 || h >= kHarm) return 0.0f;
    const float fp = clamp01 (frame01) * (float) (numFrames - 1);
    const int f0 = juce::jlimit (0, numFrames - 1, (int) fp);
    const int f1 = juce::jmin (numFrames - 1, f0 + 1);
    const float ff = fp - (float) f0;
    const float a = harm[(size_t) (f0 * kHarm + h)], b = harm[(size_t) (f1 * kHarm + h)];
    return a + (b - a) * ff;
}

void Wavetable::analyse()
{
    harm.assign ((size_t) numFrames * kHarm, 0.0f);
    juce::dsp::FFT fft (11);   // 2048
    std::vector<float> work ((size_t) kFrameLen * 2);
    for (int f = 0; f < numFrames; ++f)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        std::copy (frame (f), frame (f) + kFrameLen, work.begin());
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        float mx = 1.0e-9f;
        for (int h = 0; h < kHarm; ++h) mx = juce::jmax (mx, work[(size_t) h + 1]);
        for (int h = 0; h < kHarm; ++h) harm[(size_t) (f * kHarm + h)] = work[(size_t) h + 1] / mx;
    }
}

std::shared_ptr<Wavetable> Wavetable::fromSamples (const float* data, int length, int frameLen, const juce::String& name)
{
    auto t = std::make_shared<Wavetable>();
    t->name = name;
    if (data == nullptr || length < 4)
        return builtIn();

    if (frameLen > 0 && length >= frameLen)
    {
        const int total = length / frameLen;
        t->numFrames = juce::jlimit (1, 256, total);
        t->frames.assign ((size_t) t->numFrames * kFrameLen, 0.0f);
        for (int f = 0; f < t->numFrames; ++f)
        {
            // when more than 256 frames, pick evenly spaced ones
            const int src = total > 256 ? (int) ((long long) f * (total - 1) / 255) : f;
            const float* s = data + (size_t) src * (size_t) frameLen;
            for (int i = 0; i < kFrameLen; ++i)
            {
                const float pos = (float) i * (float) frameLen / (float) kFrameLen;
                const int i0 = (int) pos, i1 = (i0 + 1) % frameLen;
                const float fr = pos - (float) i0;
                t->frames[(size_t) (f * kFrameLen + i)] = s[i0] + (s[i1] - s[i0]) * fr;
            }
        }
    }
    else
    {
        t->numFrames = 1;
        t->frames.assign ((size_t) kFrameLen, 0.0f);
        for (int i = 0; i < kFrameLen; ++i)
        {
            const float pos = (float) i * (float) length / (float) kFrameLen;
            const int i0 = juce::jmin (length - 1, (int) pos), i1 = (i0 + 1) % length;
            const float fr = pos - (float) i0;
            t->frames[(size_t) i] = data[i0] + (data[i1] - data[i0]) * fr;
        }
    }

    // remove DC per frame, normalise the table to a 1.0 peak
    float peak = 1.0e-6f;
    for (int f = 0; f < t->numFrames; ++f)
    {
        float* fr = t->frames.data() + (size_t) f * kFrameLen;
        double mean = 0.0;
        for (int i = 0; i < kFrameLen; ++i) mean += fr[i];
        mean /= kFrameLen;
        for (int i = 0; i < kFrameLen; ++i) { fr[i] -= (float) mean; peak = juce::jmax (peak, std::abs (fr[i])); }
    }
    for (auto& v : t->frames) v /= peak;
    t->analyse();
    return t;
}

std::shared_ptr<Wavetable> Wavetable::builtIn()
{
    static std::shared_ptr<Wavetable> cached = []
    {
        constexpr int kFrames = 16, kH = 64;
        // harmonic recipes the table morphs through
        auto recipe = [] (int shape, int h)   // h is 1-based
        {
            const float fh = (float) h;
            switch (shape)
            {
                case 0: return h == 1 ? 1.0f : 0.0f;                                                   // sine
                case 1: return (h % 2) ? ((h / 2) % 2 ? -1.0f : 1.0f) / (fh * fh) : 0.0f;             // triangle
                case 2: return 1.0f / fh;                                                              // saw
                case 3: return (h % 2) ? 1.0f / fh : 0.0f;                                             // square
                case 4: return std::sin (kPi * fh * 0.2f) / fh;                                        // 20% pulse
                default:
                {
                    const float f1 = std::exp (-std::pow ((fh - 3.0f) / 1.5f, 2.0f));
                    const float f2 = 0.7f * std::exp (-std::pow ((fh - 11.0f) / 3.0f, 2.0f));
                    return (f1 + f2) / std::sqrt (fh);                                                 // vowel-ish
                }
            }
        };
        std::vector<float> raw ((size_t) kFrames * Wavetable::kFrameLen, 0.0f);
        for (int f = 0; f < kFrames; ++f)
        {
            const float pos = (float) f / (float) (kFrames - 1) * 5.0f;
            const int a = juce::jmin (4, (int) pos);
            const float fr = pos - (float) a;
            for (int h = 1; h <= kH; ++h)
            {
                const float amp = recipe (a, h) * (1.0f - fr) + recipe (a + 1, h) * fr;
                if (std::abs (amp) < 1.0e-6f) continue;
                for (int i = 0; i < Wavetable::kFrameLen; ++i)
                    raw[(size_t) (f * Wavetable::kFrameLen + i)] += amp * std::sin (kTwoPi * (float) h * (float) i / (float) Wavetable::kFrameLen);
            }
        }
        return fromSamples (raw.data(), (int) raw.size(), Wavetable::kFrameLen, "Built-in");
    }();
    return cached;
}

// ============================================================
// GridEffect
// ============================================================
GridEffect::GridEffect (int effectId)
    : id (juce::jlimit (1, kNumEffects, effectId)), d (defForId (id)), impl (makeFxImpl (id))
{
    impl->taps = &vt;
    impl->def = d;
    impl->tableRef = &table;
    impl->assetRef = &asset;
    impl->defaultData (data);
    if (d->usesWavetable()) table.store (Wavetable::builtIn().get());
    if (d->viz == VizScope) vt.longScope.assign ((size_t) VizTaps::kLongScope, 0.0f);
    for (int k = 0; k < kMaxParams; ++k) { smoothN[k] = d->p[k].def; phys[k] = unmapParam (d->p[k], d->p[k].def); }
    for (int k = 0; k < kMaxParams; ++k) { uiNorm[(size_t) k].store (smoothN[k]); uiPhys[(size_t) k].store (phys[k]); }
    mixS = d->mixDef;
}

GridEffect::~GridEffect() = default;

std::unique_ptr<GridEffect> createEffect (int effectId)
{
    if (effectId < 1 || effectId > kNumEffects) return nullptr;
    return std::make_unique<GridEffect> (effectId);
}

void GridEffect::prepare (double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    impl->sr = sr;
    impl->prepare (sr);
    // ~25 ms parameter glide, evaluated once per sub-block
    smoothCoef = 1.0f - std::exp (-(float) kChunk / (0.025f * (float) sr));
    reset();
}

void GridEffect::reset()
{
    impl->reset();
    primed = false;
    vt.scopeMid.fill (0.0f); vt.scopeSide.fill (0.0f);
    if (! vt.longScope.empty()) std::fill (vt.longScope.begin(), vt.longScope.end(), 0.0f);
}

void GridEffect::idle()
{
    impl->idle (phys, uiMode.load());
}


void GridEffect::process (float* L, float* R, int n, const SlotInput& in)
{
    if (n <= 0) return;
    const int np = d->numParams;

    float target[kMaxParams];
    for (int k = 0; k < np; ++k)
    {
        float t = in.norm != nullptr ? in.norm[k] : d->p[k].def;
        if (! d->p[k].isChoice() && in.macroDepth != nullptr)
            t += in.macro * in.macroDepth[k];
        target[k] = clamp01 (t);
    }
    const float mixTarget = clamp01 (in.mix);
    const float activeTarget = in.bypass ? 0.0f : 1.0f;

    if (! primed)
    {
        for (int k = 0; k < np; ++k) smoothN[k] = target[k];
        mixS = mixTarget; activeS = activeTarget;
        primed = true;
    }

    const double beatsPerSample = in.bpm / (60.0 * sr);

    // effect sequencer events: a gate change starts a raised-cosine ramp
    const int gateFade = juce::jmax (1, (int) std::lround (juce::jlimit (1.0f, 10.0f, in.gateFadeMs) * 0.001f * (float) sr));
    int nextEvent = 0;
    auto applyEvent = [&] (const SlotInput::GateEvent& e, bool callTrigger)
    {
        gateFrom = gateS;
        gateTo = e.on ? 1.0f : 0.0f;
        gatePos = 0; gateLen = gateFade;
        if (e.on && callTrigger && e.runBeats > 0.0) impl->trigger (e.runBeats, e.phaseBeats, in.bpm);   // runs and pads, not mode switches
    };

    // fully bypassed: audio passes untouched, the effect sleeps (tails stop)
    if (in.bypass && activeS <= 0.0f)
    {
        for (int i = 0; i < in.numGateEvents; ++i) { gateTo = in.gateEvents[i].on ? 1.0f : 0.0f; gateS = gateFrom = gateTo; gatePos = gateLen = 0; }
        freePpq += beatsPerSample * n;
        pushScope (L, R, n, L, R);
        return;
    }

    const float activeCoef = 1.0f - std::exp (-(float) kChunk / (0.010f * (float) sr));
    float inSq = 0.0f;

    FxContext ctx;
    ctx.sr = sr; ctx.bpm = in.bpm; ctx.playing = in.playing; ctx.mode = in.mode;
    ctx.beatsPerSample = beatsPerSample;
    ctx.p = phys; ctx.n = smoothN;
    ctx.table = table.load (std::memory_order_acquire);
    if (in.data != nullptr) std::copy (in.data, in.data + kDataSize, data);
    ctx.data = data;
    ctx.dataGen = in.dataGen;

    for (int off = 0; off < n;)
    {
        // events at this sample, then a chunk that ends at the next one
        while (nextEvent < in.numGateEvents && in.gateEvents[nextEvent].offset <= off)
            applyEvent (in.gateEvents[nextEvent++], true);
        int m = juce::jmin (kChunk, n - off);
        if (nextEvent < in.numGateEvents) m = juce::jmin (m, juce::jmax (1, in.gateEvents[nextEvent].offset - off));
        bool gateMoving = gatePos < gateLen || gateS < 1.0f;
        for (int i = 0; i < m; ++i)
        {
            if (gatePos < gateLen)
            {
                const float t = (float) (++gatePos) / (float) gateLen;
                gateS = gateFrom + (gateTo - gateFrom) * (0.5f - 0.5f * std::cos (t * 3.14159265f));
            }
            else gateS = gateTo;
            gateBuf[i] = gateS;
        }
        gateMoving = gateMoving || gateS < 1.0f;
        for (int k = 0; k < np; ++k)
        {
            if (d->p[k].isChoice()) smoothN[k] = target[k];
            else smoothN[k] += (target[k] - smoothN[k]) * smoothCoef;
            phys[k] = unmapParam (d->p[k], smoothN[k]);
        }
        ctx.ppq = in.playing ? in.ppq + beatsPerSample * off : freePpq;
        ctx.scL = in.scL != nullptr ? in.scL + off : nullptr;
        ctx.scR = in.scR != nullptr ? in.scR + off : nullptr;

        float* l = L + off;
        float* r = R + off;
        for (int i = 0; i < m; ++i) { dryL[i] = l[i]; dryR[i] = r[i]; inSq += 0.25f * (l[i] + r[i]) * (l[i] + r[i]); }
        // Ring: the sequencer gates the effect's input, so tails ring out
        const bool ring = in.gateRing && gateMoving;
        if (ring) for (int i = 0; i < m; ++i) { l[i] *= gateBuf[i]; r[i] *= gateBuf[i]; }

        impl->process (l, r, m, ctx);

        bool bad = false;
        for (int i = 0; i < m; ++i) if (! std::isfinite (l[i]) || ! std::isfinite (r[i])) { bad = true; break; }
        if (bad) { impl->reset(); for (int i = 0; i < m; ++i) { l[i] = dryL[i]; r[i] = dryR[i]; } }

        // per-sample ramps for mix and the bypass fade
        const float mix0 = mixS, act0 = activeS;
        mixS += (mixTarget - mixS) * smoothCoef;
        activeS += (activeTarget - activeS) * activeCoef;
        if (activeTarget <= 0.0f && activeS < 0.001f) activeS = 0.0f;
        if (activeTarget >= 1.0f && activeS > 0.999f) activeS = 1.0f;
        // mix law: effects whose wet signal is largely uncorrelated with the dry
        // one (space, time, modulation, pitch) crossfade at equal power so a
        // partial mix keeps the level; the rest (EQ, dynamics, drive ...)
        // crossfade linearly, so a parallel blend doesn't bump +3 dB
        const bool equalPower = d->cat == CatSpace || d->cat == CatTime || d->cat == CatModulation || d->cat == CatPitch;
        for (int i = 0; i < m; ++i)
        {
            const float t = (float) (i + 1) / (float) m;
            float w = (mix0 + (mixS - mix0) * t) * (act0 + (activeS - act0) * t);
            // Cut: the sequencer fades the wet signal (an off step passes dry);
            // Ring: the dry path opens fully while off, the wet tail keeps its gain
            if (gateMoving && ! ring) w *= gateBuf[i];
            const float dryOpen = ring ? 1.0f - gateBuf[i] : 0.0f;
            if (equalPower)
            {
                const float gd = std::cos (w * 1.5707963f), gw = std::sin (w * 1.5707963f);
                const float dg = dryOpen > 0.0f ? gd + (1.0f - gd) * dryOpen : gd;
                l[i] = dryL[i] * dg + l[i] * gw;
                r[i] = dryR[i] * dg + r[i] * gw;
            }
            else if (dryOpen > 0.0f)
            {
                const float dg = (1.0f - w) + w * dryOpen;
                l[i] = dryL[i] * dg + l[i] * w;
                r[i] = dryR[i] * dg + r[i] * w;
            }
            else   // (the original form: bit-identical output when the sequencer is idle)
            {
                l[i] = dryL[i] + (l[i] - dryL[i]) * w;
                r[i] = dryR[i] + (r[i] - dryR[i]) * w;
            }
        }
        freePpq += beatsPerSample * m;
        off += m;
    }

    for (int k = 0; k < kMaxParams; ++k)
    {
        uiNorm[(size_t) k].store (smoothN[k], std::memory_order_relaxed);
        uiPhys[(size_t) k].store (phys[k], std::memory_order_relaxed);
    }
    uiMode.store (in.mode, std::memory_order_relaxed);
    uiPpq.store (in.playing ? in.ppq : freePpq, std::memory_order_relaxed);
    uiBpm.store (in.bpm, std::memory_order_relaxed);
    vt.inLevelDb.store (gainToDb (std::sqrt (inSq / (float) n)), std::memory_order_relaxed);
    pushScope (L, R, n, nullptr, nullptr);
}

void GridEffect::pushScope (const float* L, const float* R, int n, const float*, const float*)
{
    int w = vt.scopeW.load (std::memory_order_relaxed);
    float rms = 0.0f, pk = 0.0f;
    const bool longScope = ! vt.longScope.empty();
    int lw = vt.longW.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        const float mid = 0.5f * (L[i] + R[i]), side = 0.5f * (L[i] - R[i]);
        vt.scopeMid[(size_t) w] = mid;
        vt.scopeSide[(size_t) w] = side;
        w = (w + 1) & (VizTaps::kScope - 1);
        if (longScope) { vt.longScope[(size_t) lw] = mid; lw = (lw + 1) & (VizTaps::kLongScope - 1); }
        rms += mid * mid;
        pk = juce::jmax (pk, std::abs (L[i]), std::abs (R[i]));
    }
    vt.scopeW.store (w, std::memory_order_relaxed);
    vt.longW.store (lw, std::memory_order_relaxed);
    vt.levelDb.store (gainToDb (std::sqrt (rms / (float) juce::jmax (1, n))), std::memory_order_relaxed);
    const float pkDb = gainToDb (pk), held = vt.peakDb.load (std::memory_order_relaxed);
    vt.peakDb.store (pkDb >= held ? pkDb : juce::jmax (pkDb, held - 0.3f), std::memory_order_relaxed);
}

} // namespace efx
