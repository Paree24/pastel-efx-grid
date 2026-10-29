// Headless DSP self-test. Dev tool only, never shipped.
//
//   stability    every effect, defaults / all-min / all-max / random settings,
//                odd block sizes: output must stay finite and bounded
//   flow         at defaults the effect passes signal (not silent) and its
//                level stays within a sane window of the input
//   dead knobs   each control, moved min -> max (with whatever mode / choice
//                makes it relevant), must change the output
//   macro        the default macro routing must change the output
//   mix / bypass mix 0 and bypass must return the dry signal exactly
//   cpu          real-time factor per effect
#include <algorithm>
#include "dsp/Effects.h"
#include "Sequencer.h"
#include <juce_core/juce_core.h>
#include <cstdio>
#include <random>
#include <functional>

using namespace efx;

namespace
{
constexpr double kSr = 48000.0;
constexpr double ClickRemoverLookaheadMs = 9.0;

// a musical-ish test signal: pulsed chord + a little noise, stereo
struct Signal
{
    long long n = 0;
    std::mt19937 rng { 7 };
    std::uniform_real_distribution<float> uni { -1.0f, 1.0f };
    float hiss = 0.0f;
    void fill (float* L, float* R, int count, float* scL = nullptr, float* scR = nullptr)
    {
        for (int i = 0; i < count; ++i, ++n)
        {
            const double t = (double) n / kSr;
            const double beatPh = std::fmod (t * 2.0, 1.0);                  // 120 bpm
            // pulsed notes with a silent gap each beat (exercises gates / envelopes)
            const float env = beatPh < 0.65 ? (float) std::exp (-beatPh * 6.0) * 0.8f + 0.2f : 0.0f;
            const float s = (float) (0.25 * std::sin (2 * juce::MathConstants<double>::pi * 110.0 * t)
                                    + 0.18 * std::sin (2 * juce::MathConstants<double>::pi * 440.0 * t)
                                    + 0.10 * std::sin (2 * juce::MathConstants<double>::pi * 1760.0 * t));
            const float nz = uni (rng) * 0.03f;
            // short bursts of hiss (sibilance) at the start of each beat
            hiss = 0.9f * hiss + 0.1f * uni (rng);
            const float sib = beatPh < 0.06 ? (uni (rng) - hiss) * 0.12f : 0.0f;
            L[i] = (s + nz) * env + nz * 0.02f + sib;
            R[i] = (s * 0.9f - nz) * env + nz * 0.02f + sib;
            if (n % 14400 == 7000) { L[i] += 0.6f; R[i] -= 0.5f; }             // a click every 0.3 s
            if (scL != nullptr) { const float k = beatPh < 0.1 ? 0.8f * (float) std::sin (2 * juce::MathConstants<double>::pi * 60.0 * t) : 0.0f; scL[i] = k; scR[i] = k; }
        }
    }
};

struct Run
{
    float norm[kMaxParams] {};
    float depth[kMaxParams] {};
    float data[kDataSize] {};
    bool hasData = false;
    float macro = 0.0f, mix = 1.0f;
    int mode = 0;
    bool bypass = false;
};

Run defaults (const EffectDef& d)
{
    Run r;
    for (int k = 0; k < kMaxParams; ++k) { r.norm[k] = d.p[k].def; r.depth[k] = d.macroDepth[k]; }
    r.mix = 1.0f;
    return r;
}

// render `blocks` blocks of `bs` samples, return interleaved output from block `skip`
std::vector<float> render (GridEffect& fx, const Run& r, int blocks, int skip, int bs = 512, std::vector<float>* dryOut = nullptr, bool idle = true)
{
    fx.reset();
    Signal sig;
    std::vector<float> L ((size_t) bs), R ((size_t) bs), scL ((size_t) bs), scR ((size_t) bs), out;
    double ppq = 0.0;
    for (int b = 0; b < blocks; ++b)
    {
        sig.fill (L.data(), R.data(), bs, scL.data(), scR.data());
        if (dryOut != nullptr && b >= skip) for (int i = 0; i < bs; ++i) { dryOut->push_back (L[(size_t) i]); dryOut->push_back (R[(size_t) i]); }
        SlotInput in;
        in.norm = r.norm; in.macroDepth = r.depth; in.macro = r.macro; in.mix = r.mix; in.mode = r.mode;
        in.bypass = r.bypass; in.bpm = 120.0; in.ppq = ppq; in.playing = true;
        in.scL = scL.data(); in.scR = scR.data();
        if (r.hasData) { in.data = r.data; in.dataGen = 1; }
        fx.process (L.data(), R.data(), bs, in);
        if (b == 2 && idle) { fx.idle(); juce::Thread::sleep (60); }   // e.g. convolution IR rebuild
        ppq += bs * 2.0 / kSr;
        if (b >= skip) for (int i = 0; i < bs; ++i) { out.push_back (L[(size_t) i]); out.push_back (R[(size_t) i]); }
    }
    return out;
}

double diffDb (const std::vector<float>& a, const std::vector<float>& b)
{
    double d2 = 0.0, a2 = 0.0;
    const size_t m = juce::jmin (a.size(), b.size());
    for (size_t i = 0; i < m; ++i) { const double d = (double) a[i] - b[i]; d2 += d * d; a2 += (double) a[i] * a[i] + (double) b[i] * b[i]; }
    return 10.0 * std::log10 (juce::jmax (1.0e-20, d2) / juce::jmax (1.0e-20, a2 * 0.5));
}

double rmsDb (const std::vector<float>& a)
{
    double s = 0.0;
    for (float v : a) s += (double) v * v;
    return 10.0 * std::log10 (juce::jmax (1.0e-20, s / std::max<size_t> (1, a.size())));
}

// controls that only change what the panel shows
bool displayOnly (const EffectDef& d, int k)
{
    const juce::String n (d.name), p (d.p[k].name);
    return n == "8-Band EQ" && p == "Analyzer";
}

// controls the generic signal can't reach, with their own targeted test
bool coveredElsewhere (const EffectDef& d, int k)
{
    const juce::String n (d.name), p (d.p[k].name);
    return n == "Tape Stop" && p == "Fit";   // only acts on a sequencer run: see "seq: tape stop Fit"

}

// make control k relevant: switch the mode / choice it depends on
void makeVisible (const EffectDef& d, Run& r, int k)
{
    const auto& s = d.p[k];
    if (s.showSrc == -2) return;
    int want = 0;
    while (want < 31 && ((s.showMask >> want) & 1u) == 0) ++want;
    if (s.showSrc == -1) r.mode = want;
    else r.norm[s.showSrc] = mapParam (d.p[s.showSrc], (float) want);
}
} // namespace

// ---------------------------------------------------------------- accuracy
namespace
{
// render a pure stereo sine through an effect at the given control values
std::vector<float> renderSine (int id, const std::vector<std::pair<const char*, float>>& set, float hz, float amp, int mode, int blocks, float mix = 1.0f)
{
    const auto& d = *defForId (id);
    GridEffect fx (id);
    fx.prepare (kSr, 512);
    Run r = defaults (d);
    r.mode = mode; r.mix = mix;
    for (auto& [name, v] : set)
        for (int k = 0; k < d.numParams; ++k)
            if (juce::String (d.p[k].name) == name) r.norm[k] = mapParam (d.p[k], v);
    std::vector<float> L (512), R (512), out;
    double ppq = 0.0; long long n = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 512; ++i, ++n) L[(size_t) i] = R[(size_t) i] = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * (double) n / kSr);
        SlotInput in; in.norm = r.norm; in.macroDepth = r.depth; in.mix = r.mix; in.mode = r.mode; in.bpm = 120.0; in.ppq = ppq; in.playing = true;
        fx.process (L.data(), R.data(), 512, in);
        ppq += 512 * 2.0 / kSr;
        for (int i = 0; i < 512; ++i) out.push_back (L[(size_t) i]);
    }
    return out;
}

// magnitude spectrum (dB) of the last 16384 samples; returns peak frequency
float peakHz (const std::vector<float>& x, float lo, float hi, float* levelAt = nullptr, float atHz = 0.0f)
{
    constexpr int order = 14, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> w ((size_t) N * 2, 0.0f);
    const size_t off = x.size() - N;
    for (int i = 0; i < N; ++i) w[(size_t) i] = x[off + (size_t) i] * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / N));
    fft.performFrequencyOnlyForwardTransform (w.data(), true);
    const float bin = (float) kSr / N;
    int best = (int) (lo / bin);
    for (int k = (int) (lo / bin); k <= (int) (hi / bin); ++k) if (w[(size_t) k] > w[(size_t) best]) best = k;
    const float a = w[(size_t) best - 1], b = w[(size_t) best], c = w[(size_t) best + 1];
    const float off2 = 0.5f * (a - c) / (a - 2.0f * b + c);
    if (levelAt != nullptr)
    {
        const int k = (int) std::lround (atHz / bin);
        float m = 0.0f; for (int j = k - 2; j <= k + 2; ++j) m = juce::jmax (m, w[(size_t) j]);
        *levelAt = gainToDb (m / juce::jmax (1.0e-9f, b));
    }
    return ((float) best + off2) * bin;
}

float peakDbOf (const std::vector<float>& x, size_t from)
{
    float p = 0.0f; for (size_t i = from; i < x.size(); ++i) p = juce::jmax (p, std::abs (x[i]));
    return gainToDb (p);
}

int checks = 0, checkFails = 0;
void expect (const char* what, float got, float want, float tol)
{
    ++checks;
    const bool ok = std::abs (got - want) <= tol;
    if (! ok) ++checkFails;
    std::printf ("  %s %-46s got %9.2f  want %9.2f (+-%g)\n", ok ? "ok  " : "FAIL", what, got, want, tol);
}
} // namespace

static int runAccuracy()
{
    std::printf ("\n== accuracy ==\n");
    float img = 0.0f;
    const float fs = peakHz (renderSine (idForName ("Frequency Shifter"), { { "Shift", 100.0f }, { "Feedback", 0.0f }, { "Tone", 20000.0f } }, 1000.0f, 0.5f, 0, 80), 500, 2000, &img, 900.0f);
    expect ("freq shifter 1 kHz +100 Hz", fs, 1100.0f, 1.0f);
    expect ("freq shifter image rejection (dB, <= -30)", juce::jmax (img, -30.0f), -30.0f, 0.01f);
    expect ("pitch shifter +7 st on 440 Hz", peakHz (renderSine (idForName ("Pitch Shifter"), { { "Semis", 7.0f } }, 440.0f, 0.5f, 0, 80), 300, 1000), 659.26f, 3.0f);
    expect ("pitch correction 450 Hz -> A4 (C maj)", peakHz (renderSine (idForName ("Pitch Correction"), { { "Speed", 0.5f } }, 450.0f, 0.5f, 0, 120), 300, 600), 440.0f, 2.5f);
    expect ("harmonizer +3rd over A4 in C maj -> C5", peakHz (renderSine (idForName ("Harmonizer"), { { "Voice 2", 0.0f } }, 440.0f, 0.5f, 0, 120), 480, 600), 523.25f, 3.0f);
    expect ("formant shifter keeps pitch (440 Hz)", peakHz (renderSine (idForName ("Formant Shifter"), { { "Formant", 5.0f } }, 440.0f, 0.5f, 0, 120), 300, 600), 440.0f, 2.5f);
    expect ("ring mod 1 kHz x 300 Hz (upper sideband)", peakHz (renderSine (idForName ("Ring Mod"), { { "Freq", 300.0f }, { "Drive", 0.0f }, { "Tone", 20000.0f } }, 1000.0f, 0.5f, 0, 80), 1100, 1500), 1300.0f, 1.5f);
    {
        const auto o = renderSine (idForName ("Compressor"), { { "Thresh", -18.0f }, { "Ratio", 3.0f }, { "Knee", 0.0f }, { "Release", 50.0f } }, 1000.0f, 0.5f, 0, 80);
        expect ("compressor -6 dBFS peak, -18 / 3:1 -> -14 dBFS", peakDbOf (o, o.size() / 2), -14.0f, 0.6f);
    }
    {
        const auto o = renderSine (idForName ("Limiter"), { { "Input", 18.0f }, { "Ceiling", -1.0f } }, 200.0f, 0.5f, 0, 60);
        expect ("limiter ceiling -1 dBFS (+12 dB over)", peakDbOf (o, 0), -1.0f, 0.01f);
    }
    {
        const auto o = renderSine (idForName ("3-Band EQ"), { { "Low", 12.0f }, { "Low Freq", 200.0f } }, 40.0f, 0.1f, 0, 60);
        expect ("3-band EQ low shelf +12 dB at 40 Hz", peakDbOf (o, o.size() / 2) - gainToDb (0.1f), 12.0f, 0.8f);
    }
    {
        const auto o = renderSine (idForName ("Gain"), { { "Gain", -6.0f } }, 1000.0f, 0.5f, 0, 20);
        expect ("gain -6 dB", peakDbOf (o, o.size() / 2) - gainToDb (0.5f), -6.0f, 0.05f);
    }
    {
        // vibrato at 100 cents peak deviation: the output spans ~ +-1 semitone
        const auto o = renderSine (idForName ("Vibrato"), { { "Depth", 100.0f }, { "Rate", 1.0f }, { "Tone", 20000.0f } }, 1000.0f, 0.5f, 0, 200);
        float mn = 1.0e9f, mx = 0.0f;
        double last = -1.0; const size_t from = o.size() / 2;
        std::vector<float> fr;
        for (size_t i = from + 1; i < o.size(); ++i)
            if (o[i - 1] <= 0.0f && o[i] > 0.0f)
            {
                const double t = (double) (i - 1) + o[i - 1] / (o[i - 1] - o[i]);   // interpolated crossing
                if (last >= 0.0) fr.push_back ((float) (kSr / (t - last)));
                last = t;
            }
        // light smoothing over 5 periods removes interpolation jitter
        for (size_t i = 2; i + 2 < fr.size(); ++i)
        {
            const float f = (fr[i - 2] + fr[i - 1] + fr[i] + fr[i + 1] + fr[i + 2]) / 5.0f;
            mn = juce::jmin (mn, f); mx = juce::jmax (mx, f);
        }
        expect ("vibrato 100 ct: max/min freq ratio (semitones)", 12.0f * std::log2 (mx / mn), 2.0f, 0.35f);
    }
    {
        // simple delay synced 1/8 at 120 bpm = 250 ms: first echo of an impulse
        GridEffect fx (idForName ("Simple Delay"));
        fx.prepare (kSr, 512);
        Run r = defaults (fx.def()); r.mix = 1.0f;
        std::vector<float> L (512), R (512);
        int hit = -1;
        for (int b = 0; b < 40 && hit < 0; ++b)
        {
            std::fill (L.begin(), L.end(), 0.0f); std::fill (R.begin(), R.end(), 0.0f);
            if (b == 0) L[0] = R[0] = 1.0f;
            SlotInput in; in.norm = r.norm; in.macroDepth = r.depth; in.mix = 1.0f; in.bpm = 120.0;
            fx.process (L.data(), R.data(), 512, in);
            for (int i = 0; i < 512; ++i) if (std::abs (L[(size_t) i]) > 0.05f) { hit = b * 512 + i; break; }
        }
        expect ("simple delay 1/8 @120 bpm (ms)", (float) hit / (float) kSr * 1000.0f, 250.0f, 1.0f);
    }
    {
        // EBU R128 / BS.1770: a 1 kHz sine at -20 dBFS peak in both channels reads -20 LUFS
        GridEffect fx (idForName ("Loudness Meter"));
        fx.prepare (kSr, 512);
        Run r = defaults (fx.def());
        std::vector<float> L (512), R (512);
        long long n = 0;
        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < 512; ++i, ++n) L[(size_t) i] = R[(size_t) i] = 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) n / kSr);
            SlotInput in; in.norm = r.norm; in.macroDepth = r.depth; in.mix = 1.0f; in.bpm = 120.0;
            fx.process (L.data(), R.data(), 512, in);
        }
        expect ("loudness: 1 kHz -20 dBFS stereo, short-term (LUFS)", fx.taps().lufsS.load(), -20.0f, 0.5f);
        expect ("loudness: integrated (LUFS)", fx.taps().lufsI.load(), -20.0f, 0.5f);
    }
    std::printf ("%d/%d accuracy checks passed\n", checks - checkFails, checks);
    return checkFails;
}

// ---------------------------------------------------------------- targeted signal tests
namespace
{
// run a mono signal (same in both channels) through an effect; returns left output
std::vector<float> runSignal (int id, const std::vector<std::pair<const char*, float>>& set, const std::vector<float>& in, int mode = 0, float mix = 1.0f)
{
    const auto& d = *defForId (id);
    GridEffect fx (id);
    fx.prepare (kSr, 512);
    Run r = defaults (d);
    r.mode = mode; r.mix = mix;
    for (auto& [name, v] : set)
    {
        bool found = false;
        for (int k = 0; k < d.numParams; ++k)
            if (juce::String (d.p[k].name) == name) { r.norm[k] = mapParam (d.p[k], v); found = true; }
        jassert (found); juce::ignoreUnused (found);
    }
    std::vector<float> out (in.size());
    std::vector<float> L (512), R (512);
    double ppq = 0.0;
    for (size_t off = 0; off < in.size(); off += 512)
    {
        const int n = (int) std::min<size_t> (512, in.size() - off);
        for (int i = 0; i < n; ++i) L[(size_t) i] = R[(size_t) i] = in[off + (size_t) i];
        SlotInput si; si.norm = r.norm; si.macroDepth = r.depth; si.mix = r.mix; si.mode = r.mode; si.bpm = 120.0; si.ppq = ppq; si.playing = true;
        fx.process (L.data(), R.data(), n, si);
        ppq += n * 2.0 / kSr;
        for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
    }
    return out;
}

double sine (double f, size_t i) { return std::sin (2.0 * juce::MathConstants<double>::pi * f * (double) i / kSr); }

// energy (dB) in a frequency band, via a 4th-order bandpass, over [from, to)
double bandDb (const std::vector<float>& x, float lo, float hi, size_t from, size_t to)
{
    Biquad h1, h2, l1, l2;
    h1.setHighPass (lo, 0.707f, kSr); h2.copyCoeffs (h1); l1.setLowPass (hi, 0.707f, kSr); l2.copyCoeffs (l1);
    double e = 0.0;
    for (size_t i = 0; i < x.size() && i < to; ++i)
    {
        const float y = l2.process (l1.process (h2.process (h1.process (x[i]))));
        if (i >= from) e += (double) y * y;
    }
    return 10.0 * std::log10 (juce::jmax (1.0e-20, e / (double) std::max<size_t> (1, to - from)));
}

// level (dB) of one exact frequency over [from, to) (Goertzel)
double toneDb (const std::vector<float>& x, double f, size_t from, size_t to)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * f / kSr, cw = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = from; i < to; ++i) { const double s0 = x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
    const double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return 10.0 * std::log10 (juce::jmax (1.0e-30, p)) - 20.0 * std::log10 ((double) (to - from) * 0.5);
}

double errDb (const std::vector<float>& a, const std::vector<float>& ref, size_t from, int delay = 0)
{
    double e = 0.0, r = 0.0;
    for (size_t i = from; i < a.size(); ++i)
    {
        const long long j = (long long) i - delay;
        if (j < 0) continue;
        const double d = (double) a[i] - ref[(size_t) j];
        e += d * d; r += (double) ref[(size_t) j] * ref[(size_t) j];
    }
    return 10.0 * std::log10 (juce::jmax (1.0e-20, e) / juce::jmax (1.0e-20, r));
}
} // namespace

static int runSignalTests()
{
    std::printf ("\n== targeted signal tests ==\n");
    const int before = checkFails;
    const size_t N = (size_t) (kSr * 4.0);
    Rng rng; rng.seed (4242);

    // ---- De-Esser: a vowel (150 Hz harmonics up to 3 kHz) plus 7 kHz sibilant noise bursts
    {
        std::vector<float> in (N);
        Biquad bp; bp.setBandPass (7000.0f, 1.2f, kSr);
        for (size_t i = 0; i < N; ++i)
        {
            double v = 0.0;
            for (int h = 1; h <= 20; ++h) v += 0.06 / h * sine (150.0 * h, i);
            const bool burst = std::fmod ((double) i / kSr, 0.4) < 0.12;
            in[i] = (float) v + bp.process (rng.bipolar()) * (burst ? 0.6f : 0.0f);
        }
        const auto out = runSignal (idForName ("De-Esser"), { { "Thresh", -40.0f }, { "Range", 12.0f } }, in);
        const double sIn = bandDb (in, 5000, 10000, N / 4, N), sOut = bandDb (out, 5000, 10000, N / 4, N);
        const double vIn = bandDb (in, 100, 2500, N / 4, N), vOut = bandDb (out, 100, 2500, N / 4, N);
        expect ("de-esser: sibilance band reduced by (dB, >= 6)", (float) juce::jmin (6.0, sIn - sOut), 6.0f, 0.01f);
        expect ("de-esser: vowel band unchanged (dB)", (float) (vOut - vIn), 0.0f, 0.5f);
    }
    // ---- Hum Remover: chord + 50 Hz hum with harmonics
    {
        std::vector<float> in (N), clean (N);
        for (size_t i = 0; i < N; ++i)
        {
            clean[i] = (float) (0.2 * sine (440.0, i) + 0.15 * sine (554.37, i) + 0.12 * sine (659.25, i));
            in[i] = clean[i] + (float) (0.1 * sine (50.0, i) + 0.05 * sine (100.0, i) + 0.04 * sine (150.0, i) + 0.03 * sine (200.0, i));
        }
        const auto out = runSignal (idForName ("Hum Remover"), {}, in, 0);
        for (double f : { 50.0, 100.0, 150.0, 200.0 })
            expect (("hum remover: " + juce::String ((int) f) + " Hz reduced by (dB, >= 30)").toRawUTF8(),
                    (float) juce::jmin (30.0, toneDb (in, f, N / 2, N) - toneDb (out, f, N / 2, N)), 30.0f, 0.01f);
        expect ("hum remover: music (400-700 Hz) unchanged (dB)", (float) (bandDb (out, 400, 700, N / 2, N) - bandDb (in, 400, 700, N / 2, N)), 0.0f, 0.5f);
    }
    // ---- Click Remover: chord with 8 single-sample clicks per second; and the same without clicks
    {
        std::vector<float> in (N), clean (N);
        for (size_t i = 0; i < N; ++i)
        {
            clean[i] = (float) (0.25 * sine (220.0, i) + 0.12 * sine (660.0, i) + 0.06 * sine (1320.0, i));
            in[i] = clean[i];
            if (i % 6000 == 1234) { in[i] += 0.6f * (rng.uni() < 0.5f ? -1.0f : 1.0f); in[i + 1] -= 0.3f * in[i] / std::abs (in[i]); }
        }
        const int lat = (int) std::ceil (ClickRemoverLookaheadMs * 0.001 * kSr) + 4;
        const auto out = runSignal (idForName ("Click Remover"), { { "Sens", 0.6f } }, in, 0);
        const double before2 = errDb (in, clean, N / 8), after = errDb (out, clean, N / 8, lat);
        expect ("click remover: click energy reduced by (dB, >= 12)", (float) juce::jmin (12.0, before2 - after), 12.0f, 0.01f);
        const auto outClean = runSignal (idForName ("Click Remover"), { { "Sens", 0.6f } }, clean, 0);
        expect ("click remover: clean signal untouched (err dB, <= -60)", (float) juce::jmax (-60.0, errDb (outClean, clean, N / 8, lat)), -60.0f, 0.01f);
        // drum-like transients must survive (no false detections)
        std::vector<float> drums (N);
        for (size_t i = 0; i < N; ++i) { const double t = std::fmod ((double) i / kSr, 0.25); drums[i] = (float) (std::exp (-t * 40.0) * (0.6 * sine (90.0 * (1.0 + 2.0 * std::exp (-t * 60.0)), i) + 0.3 * rng.bipolar())); }
        const auto outDr = runSignal (idForName ("Click Remover"), { { "Sens", 0.6f } }, drums, 0);
        expect ("click remover: drum transients kept (err dB, <= -20)", (float) juce::jmax (-20.0, errDb (outDr, drums, N / 8, lat)), -20.0f, 0.01f);
    }
    // ---- Expander: static curve, 1 kHz at -50 dBFS below -40 threshold, ratio 2 -> -60 dBFS
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (dbToGain (-50.0f) * sine (1000.0, i));
        const auto out = runSignal (idForName ("Expander"), { { "Thresh", -40.0f }, { "Ratio", 2.0f }, { "Knee", 0.0f }, { "Range", 40.0f } }, in);
        expect ("expander: -50 dB in, -40 thr, 1:2 -> out level (dBFS peak)", (float) bandDb (out, 500, 2000, N / 2, N) - (float) bandDb (in, 500, 2000, N / 2, N), -10.0f, 1.0f);
    }
    // ---- Squasher: loud and quiet sines both pulled toward the target
    {
        std::vector<float> loud (N), quiet (N);
        for (size_t i = 0; i < N; ++i) { loud[i] = (float) (0.7 * sine (300.0, i)); quiet[i] = (float) (0.02 * sine (300.0, i)); }
        const auto ol = runSignal (idForName ("Squasher"), { { "Amount", 1.0f }, { "Drive", 0.0f } }, loud);
        const auto oq = runSignal (idForName ("Squasher"), { { "Amount", 1.0f }, { "Drive", 0.0f } }, quiet);
        const double spreadIn = bandDb (loud, 100, 1000, N / 2, N) - bandDb (quiet, 100, 1000, N / 2, N);
        const double spreadOut = bandDb (ol, 100, 1000, N / 2, N) - bandDb (oq, 100, 1000, N / 2, N);
        expect ("squasher: 31 dB level spread squashed to (dB, <= 8)", (float) juce::jmax (8.0, spreadOut), 8.0f, 0.01f);
        juce::ignoreUnused (spreadIn);
    }
    // ---- Transient Shaper Decay: how long the attack boost lasts after a hit
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i)
        {
            const double t = std::fmod ((double) i / kSr, 0.5);
            in[i] = (float) (0.4 * std::exp (-t * 3.0) * sine (220.0, i));
        }
        auto boostAt = [&] (float decayMs, double ms)
        {
            const auto out = runSignal (idForName ("Transient Shaper"), { { "Attack", 1.0f }, { "Sustain", 0.0f }, { "Decay", decayMs } }, in);
            double ei = 0.0, eo = 0.0;
            for (int hit = 2; hit < 7; ++hit)
            {
                const size_t s0 = (size_t) ((hit * 0.5 + ms * 0.001) * kSr), s1 = s0 + (size_t) (0.01 * kSr);
                for (size_t i = s0; i < s1; ++i) { ei += (double) in[i] * in[i]; eo += (double) out[i] * out[i]; }
            }
            return 10.0 * std::log10 (eo / ei);
        };
        const double shortB = boostAt (10.0f, 120.0), longB = boostAt (450.0f, 120.0);
        expect ("transient: attack boost 120 ms after a hit, Decay 10 ms (dB, <= 1.5)", (float) juce::jmax (1.5, shortB), 1.5f, 0.01f);
        expect ("transient: ... with Decay 450 ms still boosted (dB, >= 3)", (float) juce::jmin (3.0, longB), 3.0f, 0.01f);
    }
    // ---- Transient Shaper Release: the tail Sustain acts on (longer = cut longer)
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i)
        {
            const double t = std::fmod ((double) i / kSr, 1.0);
            in[i] = (float) (0.5 * std::exp (-t * 4.0) * sine (220.0, i));
        }
        auto tailCut = [&] (float relMs)
        {
            const auto out = runSignal (idForName ("Transient Shaper"), { { "Attack", 0.0f }, { "Sustain", -1.0f }, { "Release", relMs } }, in);
            double ei = 0.0, eo = 0.0;
            for (int hit = 1; hit < 4; ++hit)
                for (size_t i = (size_t) ((hit + 0.35) * kSr); i < (size_t) ((hit + 0.6) * kSr); ++i) { ei += (double) in[i] * in[i]; eo += (double) out[i] * out[i]; }
            return -10.0 * std::log10 (eo / ei);
        };
        const double cutShort = tailCut (60.0f), cutLong = tailCut (1500.0f);
        expect ("transient: Release 1500 ms cuts the tail more than 60 ms (dB difference, >= 3)", (float) juce::jmin (3.0, cutLong - cutShort), 3.0f, 0.01f);
    }
    // ---- Multiband Gate: flat when open, bands gate on their own, crossovers move, sidechain keys
    {
        const int id = idForName ("Multiband Gate");
        const auto& d = *defForId (id);
        // thresholds / ranges / on per band; params by name; optional sidechain
        auto runGate = [&] (const std::vector<float>& in, const float (&th)[5], const float (&on)[5],
                            std::vector<std::pair<const char*, float>> set = {}, int mode = 0, const std::vector<float>* sc = nullptr)
        {
            GridEffect fx (id);
            fx.prepare (kSr, 512);
            Run r = defaults (d);
            r.mode = mode;
            for (auto& [name, v] : set)
                for (int k = 0; k < d.numParams; ++k) if (juce::String (d.p[k].name) == name) r.norm[k] = mapParam (d.p[k], v);
            for (int b = 0; b < 5; ++b) { r.data[b * 3] = th[b]; r.data[b * 3 + 1] = -60.0f; r.data[b * 3 + 2] = on[b]; }
            std::vector<float> out (in.size()), L (512), R (512), sl (512, 0.0f), sr2 (512, 0.0f);
            for (size_t off = 0; off < in.size(); off += 512)
            {
                const int n = (int) std::min<size_t> (512, in.size() - off);
                for (int i = 0; i < n; ++i)
                {
                    L[(size_t) i] = R[(size_t) i] = in[off + (size_t) i];
                    sl[(size_t) i] = sr2[(size_t) i] = sc != nullptr ? (*sc)[off + (size_t) i] : 0.0f;
                }
                SlotInput si; si.norm = r.norm; si.macroDepth = r.depth; si.mode = r.mode; si.data = r.data; si.dataGen = 1;
                if (sc != nullptr) { si.scL = sl.data(); si.scR = sr2.data(); }
                fx.process (L.data(), R.data(), n, si);
                for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
            }
            return out;
        };
        auto tone = [&] (double hz, double amp) { std::vector<float> v (N); for (size_t i = 0; i < N; ++i) v[i] = (float) (amp * sine (hz, i)); return v; };
        const float allOpen[5] { -80, -80, -80, -80, -80 }, allOn[5] { 1, 1, 1, 1, 1 };
        // 1. open: flat everywhere, including at the crossovers (120 / 500 / 2k / 6k)
        float worst = 0.0f;
        for (double hz : { 40.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 6000.0, 10000.0, 15000.0 })
        {
            const auto in = tone (hz, 0.3);
            worst = juce::jmax (worst, (float) std::abs (toneDb (runGate (in, allOpen, allOn), hz, N / 2, N) - toneDb (in, hz, N / 2, N)));
        }
        expect ("multiband gate: all bands open sum flat 40 Hz-15 kHz (max dev dB, <= 0.3)", juce::jmax (0.3f, worst), 0.3f, 0.01f);
        // 2. close band 3 (500 Hz-2 kHz): 1 kHz drops by its range, 4 kHz passes
        const float b3closed[5] { -80, -80, 0, -80, -80 };
        {
            const auto in1k = tone (1000.0, 0.1), in4k = tone (4000.0, 0.1);
            const double d1k = toneDb (runGate (in1k, b3closed, allOn), 1000.0, N / 2, N) - toneDb (in1k, 1000.0, N / 2, N);
            const double d4k = toneDb (runGate (in4k, b3closed, allOn), 4000.0, N / 2, N) - toneDb (in4k, 4000.0, N / 2, N);
            expect ("multiband gate: closed band 3 cuts 1 kHz, mid-band (dB, <= -15: LR4 slopes leak into the open neighbours)", (float) juce::jmax (-15.0, d1k), -15.0f, 0.01f);
            expect ("multiband gate: ... while 4 kHz in band 4 passes (dB)", (float) d4k, 0.0f, 0.6f);
            // 3. move X2 up to 1.5 kHz: 1 kHz now sits in band 2 (open) and passes
            const double moved = toneDb (runGate (in1k, b3closed, allOn, { { "X2", 3000.0f }, { "X3", 6000.0f }, { "X4", 12000.0f } }), 1000.0, N / 2, N) - toneDb (in1k, 1000.0, N / 2, N);
            expect ("multiband gate: moving X2 to 3 kHz puts 1 kHz in an open band (dB)", (float) moved, 0.0f, 0.75f);
            // 6. band 3 switched off passes even with its gate closed
            const float b3off[5] { 1, 1, 0, 1, 1 };
            const double off = toneDb (runGate (in1k, b3closed, b3off), 1000.0, N / 2, N) - toneDb (in1k, 1000.0, N / 2, N);
            expect ("multiband gate: a band switched off passes (dB)", (float) off, 0.0f, 0.3f);
        }
        // 4. band 1 opens on its own level: threshold -30 dBFS, 60 Hz at -40 stays closed, at -10 opens
        {
            const float b1[5] { -30, -80, -80, -80, -80 };   // X1 = 120 Hz: test at 40 Hz, well inside band 1
            const auto quiet = tone (40.0, 0.01), loud = tone (40.0, 0.3);
            const double dq = toneDb (runGate (quiet, b1, allOn), 40.0, N / 2, N) - toneDb (quiet, 40.0, N / 2, N);
            const double dl = toneDb (runGate (loud, b1, allOn), 40.0, N / 2, N) - toneDb (loud, 40.0, N / 2, N);
            expect ("multiband gate: band 1 below its threshold stays closed (dB, <= -30)", (float) juce::jmax (-30.0, dq), -30.0f, 0.01f);
            expect ("multiband gate: band 1 above its threshold opens (dB)", (float) dl, 0.0f, 0.3f);
        }
        // 5. Ext: band 4 keyed by the sidechain
        {
            const float b4[5] { -80, -80, -80, -30, -80 };
            const auto in = tone (4000.0, 0.1), keyLoud = tone (4000.0, 0.3), keySilent = std::vector<float> (N, 0.0f);
            const double dOpen = toneDb (runGate (in, b4, allOn, {}, 1, &keyLoud), 4000.0, N / 2, N) - toneDb (in, 4000.0, N / 2, N);
            const double dShut = toneDb (runGate (in, b4, allOn, {}, 1, &keySilent), 4000.0, N / 2, N) - toneDb (in, 4000.0, N / 2, N);
            expect ("multiband gate: Ext, loud key opens band 4 (dB)", (float) dOpen, 0.0f, 0.3f);
            expect ("multiband gate: Ext, silent key keeps band 4 closed (dB, <= -40)", (float) juce::jmax (-40.0, dShut), -40.0f, 0.01f);
        }
    }
    // ---- Talkbox: the voice's formants on the synth's pitch
    {
        const int id = idForName ("Talkbox");
        const auto& d = *defForId (id);
        // a vowel: a 120 Hz pulse through two formant resonators
        auto vowel = [&] (double f1, double f2)
        {
            std::vector<float> v (N);
            Biquad r1, r2; r1.setBandPass ((float) f1, 8.0f, kSr); r2.setBandPass ((float) f2, 10.0f, kSr);
            const int period = (int) (kSr / 120.0);
            for (size_t i = 0; i < N; ++i)
            {
                const float pulse = (i % (size_t) period) == 0 ? 1.0f : 0.0f;
                v[i] = 0.6f * r1.process (pulse) + 0.4f * r2.process (pulse);
            }
            float pk = 1e-9f; for (float x : v) pk = std::max (pk, std::abs (x));
            for (auto& x : v) x *= 0.3f / pk;
            return v;
        };
        std::vector<float> saw (N, 0.0f);
        for (size_t i = 0; i < N; ++i)
            for (int h = 1; h * 220.0 < 8000.0; ++h) saw[i] += (float) (0.25 / h * sine (220.0 * h, i));
        auto talk = [&] (const std::vector<float>& carrier, const std::vector<float>* voice)
        {
            GridEffect fx (id);
            fx.prepare (kSr, 512);
            Run r = defaults (d);
            std::vector<float> out (N), L (512), R (512), sl (512), sr2 (512);
            for (size_t off = 0; off < N; off += 512)
            {
                const int n = (int) std::min<size_t> (512, N - off);
                for (int i = 0; i < n; ++i)
                {
                    L[(size_t) i] = R[(size_t) i] = carrier[off + (size_t) i];
                    sl[(size_t) i] = sr2[(size_t) i] = voice != nullptr ? (*voice)[off + (size_t) i] : 0.0f;
                }
                SlotInput si; si.norm = r.norm; si.macroDepth = r.depth; si.mix = 1.0f; si.bpm = 120.0;
                if (voice != nullptr) { si.scL = sl.data(); si.scR = sr2.data(); }
                fx.process (L.data(), R.data(), n, si);
                for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
            }
            return out;
        };
        const auto vA = vowel (700.0, 1220.0), vI = vowel (300.0, 2300.0);
        const auto oA = talk (saw, &vA), oI = talk (saw, &vI);
        auto band = [&] (const std::vector<float>& x, float lo, float hi) { return bandDb (x, lo, hi, N / 2, N); };
        const double tiltA = band (oA, 600, 850) - band (oA, 250, 350), tiltI = band (oI, 600, 850) - band (oI, 250, 350);
        expect ("talkbox: /a/ vs /i/: F1 region 700 Hz over 300 Hz differs (dB, >= 6)", (float) std::min (6.0, tiltA - tiltI), 6.0f, 0.01f);
        expect ("talkbox: /i/ has more 2.3 kHz than /a/ (dB, >= 6)", (float) std::min (6.0, band (oI, 2100, 2500) - band (oA, 2100, 2500)), 6.0f, 0.01f);
        expect ("talkbox: the synth's pitch (220 Hz) comes through, not the voice's 120 Hz (dB, >= 20)",
                (float) std::min (20.0, toneDb (oA, 660.0, N / 2, N) - toneDb (oA, 120.0, N / 2, N)), 20.0f, 0.01f);
        const std::vector<float> silent (N, 0.0f);
        const auto oS = talk (saw, &silent);
        float pk = 0.0f; for (size_t i = N / 2; i < N; ++i) pk = std::max (pk, std::abs (oS[i]));
        expect ("talkbox: silent voice -> silent output (dBFS, <= -60)", std::max (-60.0f, gainToDb (pk)), -60.0f, 0.01f);
        const auto oN = talk (saw, nullptr);
        float diff = 0.0f; for (size_t i = 0; i < N; ++i) diff = std::max (diff, std::abs (oN[i] - saw[i]));
        expect ("talkbox: no sidechain -> the input passes (max err)", diff, 0.0f, 1.0e-6f);
    }
    // ---- Humanizer: per-step levels repeat with the bar, the average stays put
    {
        const int id = idForName ("Humanizer");
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.3 * sine (330.0, i));
        // Steps, 1/8 (0.25 s), Repeat 1 bar (2 s): level per step in bar 1 vs bar 2
        const auto out = runSignal (id, { { "Amount", 3.0f }, { "Drift", 0.0f }, { "Div", 7.0f }, { "Repeat", 1.0f }, { "Smooth", 2.0f } }, in, 1);
        auto stepDb = [&] (int k) { const size_t a0 = (size_t) ((k * 0.25 + 0.05) * kSr), a1 = (size_t) ((k * 0.25 + 0.2) * kSr);
                                    return bandDb (out, 100, 2000, a0, a1) - bandDb (in, 100, 2000, a0, a1); };
        double worst = 0.0, spread = 0.0;
        for (int k = 0; k < 8; ++k)
        {
            worst = std::max (worst, std::abs (stepDb (k) - stepDb (k + 8)));
            spread = std::max (spread, std::abs (stepDb (k)));
        }
        expect ("humanizer: Steps + Repeat 1 bar: bar 2 repeats bar 1 (max diff dB)", (float) worst, 0.0f, 0.1f);
        expect ("humanizer: ... with levels spread up to Amount (dB, >= 1)", (float) std::min (1.0, spread), 1.0f, 0.01f);
        // Free + drift: the long-term average stays at 0 dB
        const auto freeOut = runSignal (id, { { "Amount", 3.0f }, { "Drift", 3.0f }, { "Rate", 2.0f } }, in, 2);
        expect ("humanizer: average level stays put (dB)", (float) (bandDb (freeOut, 100, 2000, 0, N) - bandDb (in, 100, 2000, 0, N)), 0.0f, 0.5f);
        // Hits: each hit gets its own level
        std::vector<float> hits (N);
        for (size_t i = 0; i < N; ++i) { const double t = std::fmod ((double) i / kSr, 0.25); hits[i] = (float) (0.5 * std::exp (-t * 30.0) * sine (200.0, i)); }
        const auto hOut = runSignal (id, { { "Amount", 4.0f }, { "Drift", 0.0f }, { "Smooth", 1.0f } }, hits, 0);
        double hmin = 1e9, hmax = -1e9;
        for (int k = 2; k < 14; ++k)
        {
            const size_t a0 = (size_t) ((k * 0.25 + 0.01) * kSr), a1 = (size_t) ((k * 0.25 + 0.06) * kSr);
            const double dlt = bandDb (hOut, 100, 2000, a0, a1) - bandDb (hits, 100, 2000, a0, a1);
            hmin = std::min (hmin, dlt); hmax = std::max (hmax, dlt);
        }
        expect ("humanizer: Hits: each hit gets its own level (spread dB, >= 2)", (float) std::min (2.0, hmax - hmin), 2.0f, 0.01f);
    }
    // ---- Harmonic Resonator: notes in the set pass, others are filtered out
    {
        const int id = idForName ("Harmonic Resonator");
        auto gainFor = [&] (double hz, int mode, const std::vector<std::pair<const char*, float>>& set)
        {
            std::vector<float> in (N);
            for (size_t i = 0; i < N; ++i) in[i] = (float) (0.25 * sine (hz, i));
            const auto out = runSignal (id, set, in, mode);
            return toneDb (out, hz, N / 2, N) - toneDb (in, hz, N / 2, N);
        };
        // C major (scale mode, root C): A4 in, G#4 out
        expect ("harmonic resonator: A4 in C major passes (dB)", (float) gainFor (440.0, 0, {}), 0.0f, 1.5f);
        expect ("harmonic resonator: G#4 not in C major is cut (dB, <= -12)", (float) juce::jmax (-12.0, gainFor (415.30, 0, {})), -12.0f, 0.01f);
        expect ("harmonic resonator: more Reso cuts G#4 harder (dB, <= -20)", (float) juce::jmax (-20.0, gainFor (415.30, 0, { { "Reso", 0.95f } })), -20.0f, 0.01f);
        // chord mode, C maj: E4 passes, D4 is cut
        expect ("harmonic resonator: E4 in a C maj chord passes (dB)", (float) gainFor (329.63, 1, {}), 0.0f, 1.5f);
        expect ("harmonic resonator: D4 not in C maj chord is cut (dB, <= -12)", (float) juce::jmax (-12.0, gainFor (293.66, 1, {})), -12.0f, 0.01f);
        // root moves the set: D major has F#4
        expect ("harmonic resonator: F#4 in D major passes (dB)", (float) gainFor (369.99, 0, { { "Root", 2.0f } }), 0.0f, 1.5f);
        // custom set (defaults to C major): A passes
        expect ("harmonic resonator: custom set A4 passes (dB)", (float) gainFor (440.0, 2, {}), 0.0f, 1.5f);
        // range: below Low nothing rings
        expect ("harmonic resonator: A2 below Low 200 Hz is cut (dB, <= -20)", (float) juce::jmax (-20.0, gainFor (110.0, 0, { { "Low", 200.0f } })), -20.0f, 0.01f);
        // arp: noise in, one note of the set at a time, in the mode's order
        {
            std::vector<float> noise (N);
            juce::Random rnd (3);
            for (auto& v : noise) v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.2f;
            const double step = 0.5;   // 1/4 at 120 bpm
            auto loudest = [&] (const std::vector<float>& out, int k, const std::vector<double>& cands)
            {
                const size_t s0 = (size_t) ((k * step + step * 0.45) * kSr), s1 = (size_t) ((k * step + step * 0.9) * kSr);
                int best = 0; double bestDb = -1e9;
                for (size_t c = 0; c < cands.size(); ++c)
                {
                    const double db = toneDb (out, cands[c], s0, s1);
                    if (db > bestDb) { bestDb = db; best = (int) c; }
                }
                return best;
            };
            const std::vector<double> ceg { 261.63, 329.63, 392.0 };   // C4 E4 G4
            auto seq = [&] (float arpMode, int steps, std::vector<std::pair<const char*, float>> extra = {})
            {
                std::vector<std::pair<const char*, float>> set { { "Arp", arpMode }, { "Rate", 0.0f } };
                for (auto& e : extra) set.push_back (e);
                const auto out = runSignal (id, set, noise, 1);   // chord mode, C maj
                juce::String r;
                for (int k = 1; k <= steps; ++k) r << "CEG"[loudest (out, k, ceg)];
                return r;
            };
            const auto up = seq (1.0f, 6), down = seq (2.0f, 6), updown = seq (3.0f, 6);
            expect (("harmonic resonator arp Up: steps 1-6 = EGCEGC (got " + up + ")").toRawUTF8(), up == "EGCEGC" ? 1.0f : 0.0f, 1.0f, 0.01f);
            expect (("harmonic resonator arp Down: steps 1-6 = ECGECG (got " + down + ")").toRawUTF8(), down == "ECGECG" ? 1.0f : 0.0f, 1.0f, 0.01f);
            expect (("harmonic resonator arp UpDown: steps 1-6 = EGECEG (got " + updown + ")").toRawUTF8(), updown == "EGECEG" ? 1.0f : 0.0f, 1.0f, 0.01f);
            const auto rnd6 = seq (5.0f, 6);
            int distinct = 0; for (char ch : { 'C', 'E', 'G' }) if (rnd6.containsChar (ch)) ++distinct;
            expect (("harmonic resonator arp Random uses several notes (got " + rnd6 + ")").toRawUTF8(), (float) juce::jmin (2, distinct), 2.0f, 0.01f);
            // Played keeps the chord's own order: G9 = G B D F A -> step 1 is B; Up sorts from the root -> step 1 is A
            const std::vector<double> ba { 493.88, 440.0 };   // B4 A4
            auto second = [&] (float arpMode)
            {
                const auto out = runSignal (id, { { "Arp", arpMode }, { "Rate", 0.0f }, { "Root", 7.0f }, { "Chord", (float) [] { for (int q = 0; q < kNumChordQualities; ++q) if (juce::String (kChordQualities[q].name) == "9") return q; return 0; }() } }, noise, 1);
                return loudest (out, 1, ba);
            };
            expect ("harmonic resonator arp Played: G9 step 2 is the 3rd (B)", (float) second (6.0f), 0.0f, 0.01f);
            expect ("harmonic resonator arp Up: G9 step 2 is the 9th (A)", (float) second (1.0f), 1.0f, 0.01f);
            // Gate: a short gate leaves the end of each step quieter
            auto tail = [&] (float gate)
            {
                const auto out = runSignal (id, { { "Arp", 1.0f }, { "Rate", 0.0f }, { "Gate", gate } }, noise, 1);
                double e = 0.0;
                for (int k = 2; k < 6; ++k) for (size_t i = (size_t) ((k * step + step * 0.8) * kSr); i < (size_t) ((k * step + step * 0.98) * kSr); ++i) e += (double) out[i] * out[i];
                return 10.0 * std::log10 (e + 1e-20);
            };
            expect ("harmonic resonator arp Gate 10% vs 100%: step ends quieter (dB, >= 10)", (float) juce::jmin (10.0, tail (1.0f) - tail (0.1f)), 10.0f, 0.01f);
        }
        // drive grows harmonics from a pure A1 for the bank to pick: E3 (3rd harmonic) appears
        {
            std::vector<float> in (N);
            for (size_t i = 0; i < N; ++i) in[i] = (float) (0.3 * sine (55.0, i));
            const auto dry = runSignal (id, { { "Drive", 0.0f } }, in);
            const auto wet = runSignal (id, { { "Drive", 1.0f } }, in);
            expect ("harmonic resonator: drive adds the 3rd harmonic (E3) (dB gain, >= 20)",
                    (float) juce::jmin (20.0, toneDb (wet, 165.0, N / 2, N) - toneDb (dry, 165.0, N / 2, N)), 20.0f, 0.01f);
        }
    }
    // ---- Transient Shaper sustain: -100% must cut a decaying tail hard
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) { const double t = std::fmod ((double) i / kSr, 0.5); in[i] = (float) (std::exp (-t * 6.0) * sine (200.0, i)) * 0.7f; }
        const auto out = runSignal (idForName ("Transient Shaper"), { { "Attack", 0.0f }, { "Sustain", -1.0f } }, in);
        const size_t s0 = (size_t) (2.0 * kSr);
        const double tailIn = bandDb (in, 100, 400, s0 + (size_t) (0.2 * kSr), s0 + (size_t) (0.45 * kSr));
        const double tailOut = bandDb (out, 100, 400, s0 + (size_t) (0.2 * kSr), s0 + (size_t) (0.45 * kSr));
        expect ("transient shaper: sustain -100% cuts the tail by (dB, >= 15)", (float) juce::jmin (15.0, tailIn - tailOut), 15.0f, 0.01f);
    }
    // ---- Spring reverb distortion: a -12 dBFS sine through a fully wet spring, THD of the tail
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.25 * sine (500.0, i));
        const auto out = runSignal (idForName ("Spring Reverb"), { { "Drip", 0.0f } }, in, 0, 1.0f);
        const double fund = toneDb (out, 500.0, N / 2, N);
        const double h3 = juce::jmax (toneDb (out, 1500.0, N / 2, N), toneDb (out, 1000.0, N / 2, N));
        expect ("spring: 2nd/3rd harmonics below the fundamental by (dB, >= 40)", (float) juce::jmin (40.0, fund - h3), 40.0f, 0.01f);
    }
    // ---- Filters: full resonance must stay bounded (no clipping) yet resonate
    for (auto* name : { "Multimode Filter", "SVF Filter", "Wobble" })
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.5 * sine (1000.0, i)) + 0.1f * rng.bipolar();
        const auto out = runSignal (idForName (name), juce::String (name) == "Wobble" ? std::vector<std::pair<const char*, float>> { { "Reso", 1.0f } }
                                                                                        : std::vector<std::pair<const char*, float>> { { "Cutoff", 1000.0f }, { "Reso", 1.0f } }, in);
        float pk = 0.0f; for (size_t i = N / 2; i < N; ++i) pk = juce::jmax (pk, std::abs (out[i]));
        expect ((juce::String (name) + ": peak at full resonance (dBFS, <= +3)").toRawUTF8(), juce::jmax (3.0f, gainToDb (pk)), 3.0f, 0.01f);
    }
    {
        // squelch: the resonant peak is strong at low level
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = 0.02f * rng.bipolar();
        const auto out = runSignal (idForName ("Multimode Filter"), { { "Cutoff", 1000.0f }, { "Reso", 0.9f } }, in);
        expect ("multimode: resonance peak over the passband at low level (dB, >= 15)",
                (float) juce::jmin (15.0, bandDb (out, 900, 1100, N / 4, N) - bandDb (out, 200, 400, N / 4, N)), 15.0f, 0.01f);
    }
    // ---- Compressor guard: heavy make-up must not clip
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.9 * sine (100.0, i));
        const auto out = runSignal (idForName ("Compressor"), { { "Makeup", 24.0f } }, in);
        float pk = 0.0f; for (size_t i = N / 4; i < N; ++i) pk = juce::jmax (pk, std::abs (out[i]));
        expect ("compressor guard: +24 dB make-up peak (dBFS, <= -0.3)", juce::jmax (-0.3f, gainToDb (pk)), -0.3f, 0.01f);
    }
    // ---- Limiter: ceiling -30 dB and output gain
    {
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.5 * sine (100.0, i));
        const auto out = runSignal (idForName ("Limiter"), { { "Ceiling", -30.0f }, { "Out", 6.0f } }, in);
        float pk = 0.0f; for (size_t i = N / 4; i < N; ++i) pk = juce::jmax (pk, std::abs (out[i]));
        expect ("limiter: ceiling -30 dB + out +6 dB (dBFS peak)", gainToDb (pk), -24.0f, 0.05f);
    }
    // ---- Chord detector: triads, then add9 and five-note chords (voiced
    // like a keyboard: root in octave 3, extensions an octave up)
    {
        struct ChordCase { const char* label; int rootMidi; std::vector<int> semis; const char* quality; int harmonics = 3; };
        const ChordCase cases[] = {
            { "C major",        48, { 0, 4, 7 },          "maj" },
            { "A minor",        45, { 0, 3, 7 },          "min" },
            { "C add9",         48, { 0, 4, 7, 14 },      "add9" },
            { "G 7",            43, { 0, 4, 7, 10 },      "7" },
            { "C maj7",         48, { 0, 4, 7, 11 },      "maj7" },
            { "A m7",           45, { 0, 3, 7, 10 },      "m7" },
            { "D sus4",         50, { 0, 5, 7 },          "sus4" },
            { "E dim",          40, { 0, 3, 6 },          "dim" },
            { "G 9 (5 notes)",  43, { 0, 4, 7, 10, 14 },  "9" },
            { "D m9 (5 notes)", 50, { 0, 3, 7, 10, 14 },  "m9" },
            { "F maj9 (5 notes)", 41, { 0, 4, 7, 11, 14 }, "maj9" },
            { "C 6/9 (5 notes)", 48, { 0, 4, 7, 9, 14 },  "6/9" },
            { "E5 power chord, distorted",   40, { 0, 7 },        "5", 8 },
            { "A5 power chord + octave, distorted", 45, { 0, 7, 12 }, "5", 8 },
            { "D5 power chord, clean",       50, { 0, 7 },        "5" },
            { "G5 power chord, 3 strings",   43, { 0, 7, 12 },    "5" },
            { "C aug",           48, { 0, 4, 8 },          "aug" },
            { "B m7b5",          47, { 0, 3, 6, 10 },      "m7b5" },
            { "C dim7",          48, { 0, 3, 6, 9 },       "dim7" },
            { "C 6",             48, { 0, 4, 7, 9 },       "6" },
            { "A m6",            45, { 0, 3, 7, 9 },       "m6" },
            { "G 7sus4",         43, { 0, 5, 7, 10 },      "7sus4" },
            { "C mMaj7",         48, { 0, 3, 7, 11 },      "mMaj7" },
            { "C add11",         48, { 0, 4, 7, 17 },      "add11" },
            { "E 7#9", 40, { 0, 4, 7, 10, 15 },  "7#9" },
            { "G 7b9",           43, { 0, 4, 7, 10, 13 },  "7b9" },
            { "D 9sus4",         50, { 0, 5, 7, 10, 14 },  "9sus4" },
            { "F maj7#11",       41, { 0, 4, 7, 11, 18 },  "maj7#11" },
            { "C 7#5",           48, { 0, 4, 8, 10 },      "7#5" },
            { "A m11 (6 notes)", 45, { 0, 3, 7, 10, 14, 17 }, "m11" },
            { "G 13 (6 notes)",  43, { 0, 4, 7, 10, 14, 21 }, "13" },
            { "C maj13 (6 notes)", 48, { 0, 4, 7, 11, 14, 21 }, "maj13" },
            { "E 7alt",          40, { 0, 4, 10, 13, 15, 20 }, "7alt" },
            { "C quartal (4ths)", 48, { 0, 5, 10 },        "4ths" },
            { "C Elektra",       48, { 0, 7, 9, 13, 16 },  "Elektra" },
            { "C Petrushka",     48, { 0, 4, 7, 6 + 12, 10 + 12, 1 + 24 }, "Petrushka" },
        };
        for (const auto& cc : cases)
        {
            int wantQ = 0;
            for (int q = 0; q < kNumChordQualities; ++q) if (juce::String (kChordQualities[q].name) == cc.quality) wantQ = q;
            GridEffect fx (idForName ("Chord Detector"));
            fx.prepare (kSr, 512);
            Run r = defaults (fx.def());
            std::vector<float> L (512), R (512);
            long long n = 0;
            for (int b = 0; b < 300; ++b)
            {
                for (int i = 0; i < 512; ++i, ++n)
                {
                    double v = 0.0;
                    for (int st : cc.semis)
                    {
                        const double f = 440.0 * std::pow (2.0, (cc.rootMidi + st - 69) / 12.0);
                        for (int h = 1; h <= cc.harmonics; ++h) v += 0.1 / h * std::sin (2.0 * juce::MathConstants<double>::pi * f * h * (double) n / kSr);
                    }
                    L[(size_t) i] = R[(size_t) i] = (float) v;
                }
                SlotInput si; si.norm = r.norm; si.macroDepth = r.depth; si.bpm = 120.0;
                fx.process (L.data(), R.data(), 512, si);
            }
            const int detRoot = (int) fx.taps().get (0), detQ = (int) fx.taps().get (1);
            const int wantRoot = cc.rootMidi % 12;
            const juce::String got = detRoot < 0 ? juce::String ("none") : juce::String (detRoot) + " " + kChordQualities[juce::jlimit (0, kNumChordQualities - 1, detQ)].name;
            expect ((juce::String ("chord detector: ") + cc.label + " -> " + juce::String (wantRoot) + " " + cc.quality + " (got " + got + ")").toRawUTF8(),
                    (float) (detRoot * 100 + detQ), (float) (wantRoot * 100 + wantQ), 0.01f);
        }
    }
    return checkFails - before;
}


// ---------------------------------------------------------------- effect sequencer
namespace
{
// run the scheduler over consecutive host blocks; returns absolute sample positions of edges
struct Edge { long long at; bool on; double run, phase; };
std::vector<Edge> scheduleRun (const seq::LaneClock& c, double ppq0, double bpm, int block, long long total, bool startOpen = false)
{
    std::vector<Edge> out;
    const double bps = bpm / (60.0 * kSr);
    bool open = startOpen;
    seq::GateEvent ev[seq::kMaxEvents];
    for (long long pos = 0; pos < total; pos += block)
    {
        const int n = (int) std::min<long long> (block, total - pos);
        const int cnt = seq::laneEvents (c, ppq0 + bps * (double) pos, bps, n, open, ev, seq::kMaxEvents);
        for (int i = 0; i < cnt; ++i) out.push_back ({ pos + ev[i].offset, ev[i].on, ev[i].runBeats, ev[i].phaseBeats });
    }
    return out;
}
}

static int runSequencerTests()
{
    std::printf ("\n== effect sequencer ==\n");
    const int before = checkFails;

    // 1. edge timing: every gate edge within 1 sample of its exact time
    {
        const uint32_t mask = 0b1011000111001101u;   // runs of 1, 2 and 3 steps
        int bad = 0, cases = 0, edges = 0;
        for (double bpm : { 60.0, 120.5, 174.0 })
            for (int rate = 0; rate < seq::kNumRates; ++rate)
                for (double swing : { 0.0, 0.3 })
                    for (int block : { 1, 31, 64, 512, 4096 })
                    {
                        seq::Lane l; l.steps = mask;
                        seq::SceneSettings sc; sc.steps = 16; sc.rate = rate; sc.swing = (float) swing;
                        const auto c = seq::LaneClock::make (l, sc);
                        const int steps = rate < 9 ? 18 : 34;          // long rates: fewer steps (keeps the run short)
                        const double samplesPerBeat = kSr * 60.0 / bpm;
                        const long long total = (long long) std::ceil (c.stepStart (steps) * samplesPerBeat);
                        if (block == 1 && total > 400000) continue;
                        ++cases;
                        const auto got = scheduleRun (c, 0.0, bpm, block, total);
                        // expected: every step boundary where the bit changes (starting closed)
                        std::vector<std::pair<double, bool>> want;
                        bool prev = false;
                        // (an edge lands on the first sample at or after its exact time, inside the run)
                        for (long long k = 0; std::ceil (c.stepStart (k) * samplesPerBeat - 1.0e-6) < (double) total; ++k)
                            if (c.bit (k) != prev) { prev = c.bit (k); want.push_back ({ c.stepStart (k) * samplesPerBeat, prev }); }
                        if (got.size() != want.size())
                        {
                            if (bad < 3) std::printf ("    count mismatch bpm %.1f rate %s swing %.1f block %d: got %d want %d\n", bpm, seq::rateName (rate), swing, block, (int) got.size(), (int) want.size());
                            ++bad; continue;
                        }
                        for (size_t i = 0; i < got.size(); ++i)
                        {
                            ++edges;
                            if (got[i].on != want[i].second || std::abs ((double) got[i].at - want[i].first) > 1.0)
                            {
                                if (bad < 3) std::printf ("    edge %d bpm %.1f rate %s swing %.1f block %d: got %lld (%d) want %.2f (%d)\n", (int) i, bpm, seq::rateName (rate), swing, block, got[i].at, (int) got[i].on, want[i].first, (int) want[i].second);
                                ++bad; break;
                            }
                        }
                    }
        std::printf ("  (%d cases, %d edges)\n", cases, edges);
        expect ("seq: gate edges within 1 sample (rates x tempos x blocks x swing)", (float) bad, 0.0f, 0.01f);
    }
    // 2. runs and wrap: steps 14 15 0 1 on: one gate per loop, open across the loop point
    {
        seq::Lane l; l.steps = (1u << 14) | (1u << 15) | 1u | 2u;
        const auto c = seq::LaneClock::make (l, seq::SceneSettings {});
        const double spb = kSr * 60.0 / 120.0;
        const auto got = scheduleRun (c, 0.0, 120.0, 512, (long long) (c.stepStart (48) * spb));
        // open at sample 0 (steps 0-1), then one run per loop starting at step 14
        int ons = 0, offs = 0, atLoop = 0;
        double runAt14 = 0.0;
        for (auto& e : got)
        {
            (e.on ? ons : offs)++;
            for (int loop = 1; loop < 3; ++loop) if (std::llabs (e.at - (long long) std::ceil (c.stepStart (16 * loop) * spb)) <= 1) ++atLoop;
            if (e.on && std::llabs (e.at - (long long) std::ceil (c.stepStart (14) * spb)) <= 1) runAt14 = e.run;
        }
        expect ("seq: wrap run: no gate change at the loop point", (float) atLoop, 0.0f, 0.01f);
        expect ("seq: wrap run: gate-ons (start + 3 loops)", (float) ons, 4.0f, 0.01f);
        expect ("seq: wrap run: gate-offs (one per loop)", (float) offs, 3.0f, 0.01f);
        expect ("seq: wrap run length (beats, 4 x 1/16)", (float) runAt14, 1.0f, 1e-4f);
    }
    // 3. host jump into the middle of a run opens at once, with the phase gone
    {
        seq::Lane l; l.steps = 0b11110000u;   // steps 4..7
        const auto c = seq::LaneClock::make (l, seq::SceneSettings {});
        const auto got = scheduleRun (c, 5.5 * 0.25, 120.0, 512, 2000);
        expect ("seq: jump into a run opens at sample 0", (float) (got.empty() ? -1 : got[0].at), 0.0f, 0.01f);
        expect ("seq: ... with the elapsed phase (beats)", (float) (got.empty() ? -1 : got[0].phase), 0.375f, 1e-3f);
        expect ("seq: ... and the full run length (beats)", (float) (got.empty() ? -1 : got[0].run), 1.0f, 1e-3f);
    }
    // 4. polyrhythm: a 5-step and a 16-step lane line up again every 80 steps
    {
        seq::Lane a; a.steps = 0b10011u; a.len = 5;
        seq::Lane b; b.steps = 0x8421u;
        const auto ca = seq::LaneClock::make (a, seq::SceneSettings {}), cb = seq::LaneClock::make (b, seq::SceneSettings {});
        int mismatch = 0, earlier = 0;
        for (long long k = 0; k < 160; ++k)
        {
            if (ca.bit (k) != ca.bit (k + 80) || cb.bit (k) != cb.bit (k + 80)) ++mismatch;
            if (k > 0 && k < 80 && ca.bit (k) == ca.bit (0) && cb.bit (k) == cb.bit (0) && ca.bit (k + 1) == ca.bit (1) && cb.bit (k + 1) == cb.bit (1)
                && ca.bit (k + 2) == ca.bit (2) && cb.bit (k + 2) == cb.bit (2) && ca.bit (k + 3) == ca.bit (3) && cb.bit (k + 3) == cb.bit (3)
                && ca.bit (k + 4) == ca.bit (4) && cb.bit (k + 4) == cb.bit (4) && ca.bit (k + 5) == ca.bit (5) && cb.bit (k + 5) == cb.bit (5)) ++earlier;
        }
        expect ("seq: 5- and 16-step lanes repeat together after 80 steps", (float) mismatch, 0.0f, 0.01f);
        expect ("seq: euclid(16, 5) has 5 pulses", (float) juce::countNumberOfBits (seq::euclid (16, 5, 0)), 5.0f, 0.01f);
        juce::ignoreUnused (earlier);
    }
    // 5. GridEffect: Cut lane returns the input bit-exactly while off; Ring keeps a delay tail
    auto runGated = [&] (const char* name, const std::vector<float>& in, const std::vector<std::pair<int, bool>>& edges, bool ring,
                         std::vector<std::pair<const char*, float>> set = {}, float fadeMs = 3.0f, float mix = -1.0f)
    {
        const int id = idForName (name);
        const auto& d = *defForId (id);
        GridEffect fx (id);
        fx.prepare (kSr, 512);
        Run r = defaults (d);
        if (mix >= 0.0f) r.mix = mix;
        for (auto& [nm, v] : set) for (int k = 0; k < d.numParams; ++k) if (juce::String (d.p[k].name) == nm) r.norm[k] = mapParam (d.p[k], v);
        std::vector<float> out (in.size()), L (512), R (512);
        size_t e = 0;
        for (size_t off = 0; off < in.size(); off += 512)
        {
            const int n = (int) std::min<size_t> (512, in.size() - off);
            for (int i = 0; i < n; ++i) L[(size_t) i] = R[(size_t) i] = in[off + (size_t) i];
            SlotInput::GateEvent ev[32]; int cnt = 0;
            while (e < edges.size() && edges[e].first < (int) (off + (size_t) n))
            {
                ev[cnt].offset = edges[e].first - (int) off; ev[cnt].on = edges[e].second; ev[cnt].runBeats = 1.0; ++cnt; ++e;
            }
            SlotInput si; si.norm = r.norm; si.macroDepth = r.depth; si.mix = r.mix; si.bpm = 120.0; si.ppq = (double) off / kSr * 2.0; si.playing = true;
            si.gateEvents = ev; si.numGateEvents = cnt; si.gateRing = ring; si.gateFadeMs = fadeMs;
            fx.process (L.data(), R.data(), n, si);
            for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
        }
        return out;
    };
    {
        const size_t N = (size_t) kSr * 2;
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.4 * sine (440.0, i));
        // off from 0.5 s on (3 ms fade), check from 0.52 s
        const auto out = runGated ("Bitcrusher", in, { { (int) (kSr / 2), false } }, false, { { "Bits", 4.0f } });
        double worst = 0.0, onDiff = 0.0;
        for (size_t i = (size_t) (0.52 * kSr); i < N; ++i) worst = std::max (worst, (double) std::abs (out[i] - in[i]));
        for (size_t i = (size_t) (0.1 * kSr); i < (size_t) (0.45 * kSr); ++i) onDiff = std::max (onDiff, (double) std::abs (out[i] - in[i]));
        expect ("seq: Cut lane off = input bit-exact (max err)", (float) worst, 0.0f, 0.0f);
        expect ("seq: ... and the effect plays while on (crushed, err > 0.01)", onDiff > 0.01 ? 1.0f : 0.0f, 1.0f, 0.01f);
    }
    {
        // Ring: a burst while on, then a different tone after the gate closes
        const size_t N = (size_t) kSr * 3;
        std::vector<float> in (N, 0.0f);
        for (size_t i = 0; i < (size_t) (0.2 * kSr); ++i) in[i] = (float) (0.4 * sine (500.0, i));
        for (size_t i = (size_t) (0.6 * kSr); i < N; ++i) in[i] = (float) (0.4 * sine (1300.0, i));
        const auto out = runGated ("Simple Delay", in, { { (int) (0.3 * kSr), false } }, true, { { "Sync", 0.0f }, { "Time", 250.0f }, { "Feedback", 0.5f } }, 3.0f, 1.0f);
        std::vector<float> wet (N);
        for (size_t i = 0; i < N; ++i) wet[i] = out[i] - in[i];
        const double tail = toneDb (wet, 500.0, (size_t) (0.45 * kSr), (size_t) (0.75 * kSr));
        const double leak = toneDb (wet, 1300.0, (size_t) (1.0 * kSr), N);
        const double dry = toneDb (out, 1300.0, (size_t) (1.0 * kSr), N) - toneDb (in, 1300.0, (size_t) (1.0 * kSr), N);
        expect ("seq: Ring lane keeps the delay tail after closing (dBFS, >= -40)", (float) std::max (-40.0, tail), -40.0f, 40.0f);
        expect ("seq: ... new input isn't echoed (dBFS, <= -80)", (float) std::max (-80.0, leak), -80.0f, 0.01f);
        expect ("seq: ... and the dry path is fully open (dB)", (float) dry, 0.0f, 0.1f);
    }
    {
        // no clicks: a lane toggling every 1/16 on a crushed sine, at every fade
        const size_t N = (size_t) kSr * 2;
        std::vector<float> in (N);
        for (size_t i = 0; i < N; ++i) in[i] = (float) (0.4 * sine (220.0, i));
        float worst = 0.0f;
        for (float fade : { 1.0f, 3.0f, 10.0f })
        {
            std::vector<std::pair<int, bool>> edges;
            for (int k = 1; k < 30; ++k) edges.push_back ({ (int) (k * 0.0625 * kSr), (k % 2) == 0 });
            const auto out = runGated ("Bitcrusher", in, edges, false, { { "Bits", 3.0f } }, fade);
            const auto ref = runGated ("Bitcrusher", in, {}, false, { { "Bits", 3.0f } }, fade);
            // the largest step the gate adds beyond what the crushed and the dry signal have on their own
            float maxIn = 0.0f, maxRef = 0.0f, maxOut = 0.0f;
            for (size_t i = 1; i < N; ++i)
            {
                maxIn = std::max (maxIn, std::abs (in[i] - in[i - 1]));
                maxRef = std::max (maxRef, std::abs (ref[i] - ref[i - 1]));
                maxOut = std::max (maxOut, std::abs (out[i] - out[i - 1]));
            }
            worst = std::max (worst, maxOut - std::max (maxIn, maxRef));
        }
        expect ("seq: gate edges add no clicks (excess step, <= 0.01)", std::max (0.01f, worst), 0.01f, 0.001f);
    }
    // 6. triggered time effects: known answers
    {
        struct Trig { int at; bool on; double run, phase; };
        // run an effect with gate events; returns the output and the meter 0 at `probeAt`
        auto runTrig = [&] (const char* name, std::vector<std::pair<const char*, float>> set, const std::vector<float>& in,
                            std::vector<Trig> evs, int probeAt, float& probe)
        {
            const int id = idForName (name);
            const auto& d = *defForId (id);
            GridEffect fx (id);
            fx.prepare (kSr, 512);
            Run r = defaults (d);
            r.mix = 1.0f;
            for (auto& [nm, v] : set) for (int k = 0; k < d.numParams; ++k) if (juce::String (d.p[k].name) == nm) r.norm[k] = mapParam (d.p[k], v);
            std::vector<float> out (in.size()), L (256), R (256);
            size_t e = 0;
            probe = 0.0f;
            for (size_t off = 0; off < in.size(); off += 256)
            {
                const int n = (int) std::min<size_t> (256, in.size() - off);
                for (int i = 0; i < n; ++i) L[(size_t) i] = R[(size_t) i] = in[off + (size_t) i];
                SlotInput::GateEvent ev[8]; int cnt = 0;
                while (e < evs.size() && evs[e].at < (int) (off + (size_t) n))
                {
                    ev[cnt].offset = evs[e].at - (int) off; ev[cnt].on = evs[e].on; ev[cnt].runBeats = evs[e].run; ev[cnt].phaseBeats = evs[e].phase; ++cnt; ++e;
                }
                SlotInput si; si.norm = r.norm; si.macroDepth = r.depth; si.mix = 1.0f; si.bpm = 120.0; si.ppq = (double) off / kSr * 2.0; si.playing = true;
                si.gateEvents = ev; si.numGateEvents = cnt; si.gateFadeMs = 1.0f;
                fx.process (L.data(), R.data(), n, si);
                for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
                if ((int) off <= probeAt && probeAt < (int) off + n) probe = fx.taps().get (0);
            }
            return out;
        };
        const size_t N = (size_t) kSr * 4;
        std::vector<float> noise (N);
        juce::Random rnd (11);
        for (auto& v : noise) v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.3f;
        float probe = 0.0f;
        const int t0 = (int) kSr;   // beat 2 at 120 bpm
        // Stutter: run of 2 beats at Div 1/16 (6000 samples): the second Div repeats the first exactly
        {
            const auto out = runTrig ("Stutter", { { "Div", 10.0f }, { "Chance", 0.0f }, { "Repeats", 16.0f }, { "Fade", 0.1f } }, noise,
                                      { { 0, false, 0, 0 }, { t0, true, 2.0, 0.0 } }, 0, probe);
            double err = 0.0;
            for (int k = 200; k < 5800; ++k) err = std::max (err, (double) std::abs (out[(size_t) (t0 + 6000 + k)] - noise[(size_t) (t0 + k)]));
            expect ("seq: stutter repeats the captured Div exactly (max err)", (float) err, 0.0f, 1.0e-5f);
        }
        // Reverse (free): run of 0.5 beats (0.25 s): plays the audio just before the run backwards
        {
            const auto out = runTrig ("Reverse", { { "Sync", 0.0f }, { "Fade", 0.01f }, { "Feedback", 0.0f }, { "Width", 0.0f } }, noise,
                                      { { 0, false, 0, 0 }, { t0, true, 0.5, 0.0 } }, 0, probe);
            double err = 0.0;
            const int len = (int) (0.25 * kSr);
            for (int k = len / 50; k < len - len / 50; ++k) err = std::max (err, (double) std::abs (out[(size_t) (t0 + k)] - noise[(size_t) (t0 - 1 - k)]));
            expect ("seq: reverse plays the run's past backwards (max err)", (float) err, 0.0f, 1.0e-4f);
        }
        // Tape Stop with Fit: stopped by the end of a 1-beat run; without Fit (800 ms) still fast
        {
            const int endAt = t0 + (int) (0.47 * kSr);
            float rateFit = 1.0f, rateNo = 0.0f;
            runTrig ("Tape Stop", { { "Fit", 1.0f }, { "Curve", 0.0f } }, noise, { { 0, false, 0, 0 }, { t0, true, 1.0, 0.0 } }, endAt, rateFit);
            runTrig ("Tape Stop", { { "Fit", 0.0f }, { "Curve", 0.0f } }, noise, { { 0, false, 0, 0 }, { t0, true, 1.0, 0.0 } }, endAt, rateNo);
            expect ("seq: tape stop Fit: nearly stopped at the run's end (rate, <= 0.1)", std::max (0.1f, rateFit), 0.1f, 0.001f);
            expect ("seq: ... without Fit still running (rate, >= 0.3)", std::min (0.3f, rateNo), 0.3f, 0.001f);
        }
        // Buffer Shuffler: the same run twice (identical input around it) shuffles identically
        {
            std::vector<float> tone (N);
            for (size_t i = 0; i < N; ++i) tone[i] = (float) (0.3 * sine (220.0, i) + 0.1 * sine (1650.0, i));   // repeats every 1 s exactly
            const int t1 = t0 + (int) (2 * kSr);
            const auto out = runTrig ("Buffer Shuffler", { { "Chance", 1.0f }, { "Range", 0.0f } }, tone,
                                      { { 0, false, 0, 0 }, { t0, true, 1.0, 0.0 }, { t0 + (int) (0.5 * kSr), false, 0, 0 },
                                        { t1, true, 1.0, 0.0 }, { t1 + (int) (0.5 * kSr), false, 0, 0 } }, 0, probe);
            double err = 0.0;
            for (int k = (int) (0.01 * kSr); k < (int) (0.45 * kSr); ++k) err = std::max (err, (double) std::abs (out[(size_t) (t0 + k)] - out[(size_t) (t1 + k)]));   // (after the 3 ms entry crossfade)
            expect ("seq: shuffler repeats the same shuffle each run (max err)", (float) err, 0.0f, 1.0e-4f);
        }
        // Time Stretch: its loop restarts at the run (cycle position ~0 right after)
        {
            const int tMid = (int) (2.3 * 0.5 * kSr);   // beat 2.3, mid-cycle
            runTrig ("Time Stretch", {}, noise, { { 0, false, 0, 0 }, { tMid, true, 4.0, 0.0 } }, tMid + 300, probe);
            expect ("seq: time stretch restarts its loop at the run (cycle pos, <= 0.02)", std::max (0.02f, probe), 0.02f, 0.001f);
        }
    }
    return checkFails - before;
}

// CI: report a crash with a stack trace instead of dying silently
static void reportCrash (void*)
{
    std::printf ("\nCRASH\n%s\n", juce::SystemStats::getStackBacktrace().toRawUTF8());
    std::fflush (stdout);
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);   // unbuffered: CI keeps the output even if we crash
    juce::SystemStats::setApplicationCrashHandler (reportCrash);
    if (argc > 1 && juce::String (argv[1]) == "seq") return runSequencerTests() == 0 ? 0 : 1;   // just the sequencer section
    int failures = 0, warnings = 0;
    std::mt19937 rng (12345);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);

    std::printf ("== stability ==\n");
    for (int id = 1; id <= kNumEffects; ++id)
    {
        const auto& d = *defForId (id);
        GridEffect fx (id);
        fx.prepare (kSr, 1024);
        float peak = 0.0f;
        bool bad = false;
        static const int sizes[] = { 512, 1, 33, 1024, 7, 256 };
        for (int t = 0; t < 5 && ! bad; ++t)
        {
            Run r = defaults (d);
            for (int k = 0; k < d.numParams; ++k)
                r.norm[k] = t == 0 ? d.p[k].def : (t == 1 ? 0.0f : (t == 2 ? 1.0f : uni (rng)));
            r.macro = t >= 3 ? uni (rng) : (t == 2 ? 1.0f : 0.0f);
            r.mode = d.numModes > 0 ? (t % d.numModes) : 0;
            Signal sig;
            std::vector<float> L (1024), R (1024);
            double ppq = 0.0;
            for (int b = 0; b < 120 && ! bad; ++b)
            {
                const int bs = sizes[b % 6];
                sig.fill (L.data(), R.data(), bs);
                if (b == 3) { L[0] = 1.0f; R[0] = -1.0f; }
                SlotInput in;
                in.norm = r.norm; in.macroDepth = r.depth; in.macro = r.macro; in.mix = 1.0f; in.mode = r.mode;
                in.bpm = 90.0 + 60.0 * t; in.ppq = ppq; in.playing = (t % 2) == 0;
                fx.process (L.data(), R.data(), bs, in);
                ppq += bs * in.bpm / 60.0 / kSr;
                for (int i = 0; i < bs; ++i)
                {
                    if (! std::isfinite (L[(size_t) i]) || ! std::isfinite (R[(size_t) i])) { bad = true; break; }
                    peak = juce::jmax (peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]));
                }
                if (peak > 64.0f) bad = true;   // gains up to +24 dB are legitimate; runaway feedback goes far beyond
            }
            if (bad)
            {
                std::printf ("     settings (t=%d, mode %d, macro %.3f):", t, r.mode, r.macro);
                for (int k = 0; k < d.numParams; ++k) std::printf (" %s=%.4g", d.p[k].name, unmapParam (d.p[k], r.norm[k]));
                std::printf ("\n");
            }
        }
        if (bad) { std::printf ("FAIL %2d %-20s unstable (peak %.1f)\n", id, d.name, peak); ++failures; }
    }

    std::printf ("\n== flow / level at defaults ==\n");
    for (int id = 1; id <= kNumEffects; ++id)
    {
        const auto& d = *defForId (id);
        if (d.cat == CatAnalysis) continue;
        GridEffect fx (id);
        fx.prepare (kSr, 512);
        std::vector<float> dry;
        const auto out = render (fx, defaults (d), 300, 200, 512, &dry);
        const double lv = rmsDb (out) - rmsDb (dry);
        if (lv < -40.0) { std::printf ("FAIL %2d %-20s near-silent (%.1f dB)\n", id, d.name, lv); ++failures; }
        else if (lv > 9.0 || lv < -18.0) { std::printf ("warn %2d %-20s level change %+.1f dB\n", id, d.name, lv); ++warnings; }
        float pk = 0.0f;
        for (float x : out) pk = juce::jmax (pk, std::abs (x));
        // the transient shaper's job is to boost the test signal's synthetic impulses
        if (pk > 1.6f && juce::String (d.name) != "Transient Shaper") { std::printf ("warn %2d %-20s peaks at %+.1f dBFS at defaults\n", id, d.name, gainToDb (pk)); ++warnings; }
    }

    std::printf ("\n== dead controls / macro / mix / bypass ==\n");
    int dead = 0;
    for (int id = 1; id <= kNumEffects; ++id)
    {
        const auto& d = *defForId (id);
        GridEffect fx (id);
        fx.prepare (kSr, 512);
        if (d.cat != CatAnalysis)
        {
            // a control is dead only if it changes nothing in any of several
            // contexts: the defaults and a few random settings of the others
            const int blocks = d.cat == CatTime ? 520 : 200, skip = d.cat == CatTime ? 100 : 60;
            rng.seed ((unsigned) (12345 + id * 7919));   // per effect: other effects' controls don't shift its contexts
            std::vector<Run> contexts { defaults (d) };
            for (int c = 0; c < 7; ++c)
            {
                Run r = defaults (d);
                for (int k = 0; k < d.numParams; ++k) r.norm[k] = uni (rng);
                if (juce::String (d.name) == "8-Band EQ")
                {
                    // give the parametric bands some gain so Scale / Mid / Side have something to act on
                    fx.defaultData (r.data);
                    for (int b = 0; b < Shape::kEqBands; ++b) r.data[b * Shape::kEqStride + 3] = (uni (rng) * 2.0f - 1.0f) * 12.0f;
                    r.hasData = true;
                }
                r.mode = d.numModes > 0 ? c % d.numModes : 0;   // cover every mode
                contexts.push_back (r);
            }
            auto anyDiff = [&] (const std::function<void (Run&, Run&)>& vary)
            {
                double best = -300.0;
                for (const auto& ctx : contexts)
                {
                    Run a = ctx, b = ctx;
                    vary (a, b);
                    best = juce::jmax (best, diffDb (render (fx, a, blocks, skip), render (fx, b, blocks, skip)));
                    if (best > -50.0) break;
                }
                return best;
            };
            for (int k = 0; k < d.numParams; ++k)
            {
                if (displayOnly (d, k) || coveredElsewhere (d, k)) continue;
                double dd = anyDiff ([&] (Run& a, Run& b) { makeVisible (d, a, k); makeVisible (d, b, k); a.norm[k] = 0.0f; b.norm[k] = 1.0f; });
                if (dd < -50.0 && d.p[k].isChoice() && d.p[k].numChoices > 2)   // e.g. the last option may fall back to the first
                    dd = anyDiff ([&] (Run& a, Run& b) { makeVisible (d, a, k); makeVisible (d, b, k); a.norm[k] = 0.0f; b.norm[k] = mapParam (d.p[k], 1.0f); });
                if (dd < -50.0) { std::printf ("DEAD %2d %-20s : %s (%.0f dB)\n", id, d.name, d.p[k].name, dd); ++dead; }
            }
            for (int m = 1; m < d.numModes; ++m)
                if (anyDiff ([&] (Run& a, Run& b) { a.mode = 0; b.mode = m; }) < -50.0) { std::printf ("DEAD %2d %-20s : mode %s\n", id, d.name, d.modes[m]); ++dead; }
            if (anyDiff ([&] (Run& a, Run& b) { a.macro = 0.0f; b.macro = 1.0f; }) < -50.0) { std::printf ("DEAD %2d %-20s : macro\n", id, d.name); ++dead; }
        }
        // mix 0 and bypass must be bit-exact dry (after the fades settle)
        for (int which = 0; which < 2; ++which)
        {
            Run r = defaults (d);
            if (which == 0) r.mix = 0.0f; else r.bypass = true;
            std::vector<float> dry;
            const auto out = render (fx, r, 40, 10, 512, &dry);
            if (diffDb (out, dry) > -120.0) { std::printf ("FAIL %2d %-20s %s is not dry\n", id, d.name, which == 0 ? "mix 0" : "bypass"); ++failures; }
        }
    }
    std::printf ("%d dead control(s)\n", dead);
    failures += dead;

    std::printf ("\n== cpu (x real-time, 16 slots of one effect must stay well under 1) ==\n");
    std::vector<std::pair<double, int>> cost;
    for (int id = 1; id <= kNumEffects; ++id)
    {
        const auto& d = *defForId (id);
        GridEffect fx (id);
        fx.prepare (kSr, 512);
        Run r = defaults (d);
        const auto t0 = juce::Time::getHighResolutionTicks();
        render (fx, r, 94, 94, 512, nullptr, false);   // ~1 s
        const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        cost.push_back ({ sec / (94.0 * 512.0 / kSr), id });
    }
    std::sort (cost.rbegin(), cost.rend());
    for (int i = 0; i < 8; ++i) std::printf ("  %-20s %.4f\n", defForId (cost[(size_t) i].second)->name, cost[(size_t) i].first);

    failures += runAccuracy();
    failures += runSignalTests();
    failures += runSequencerTests();
    std::printf ("\n%d failure(s), %d warning(s)\n", failures, warnings);
    return failures == 0 ? 0 : 1;
}
