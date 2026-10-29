#pragma once
#include "Dsp.h"
#include "EffectRegistry.h"
#include <array>
#include <atomic>
#include <memory>

// ============================================================
// GridEffect — the DSP behind one grid slot. The plugin rebuilds the
// instance whenever the slot's effect changes, so the effect is fixed for the
// lifetime of an instance.
//
// GridEffect owns the parts every effect shares: 32-sample sub-blocks,
// parameter smoothing, the user-assigned macro, dry/wet mix, a click-free
// bypass, a tempo clock and the taps the visualisers read. Each effect is an
// FxImpl (one class per effect, see fx/*.cpp) that only sees physical values.
// ============================================================

namespace efx
{

// ---------------------------------------------------------------
// Wavetable: frames of one single-cycle waveform each, plus each frame's
// harmonic magnitudes. Immutable once built (built on the message thread).
struct Wavetable
{
    static constexpr int kFrameLen = 2048;
    static constexpr int kHarm = 48;
    std::vector<float> frames;   // numFrames * kFrameLen
    std::vector<float> harm;     // numFrames * kHarm, max-normalised per frame
    int numFrames = 0;
    juce::String name;

    float sample (float frame01, float phase01) const;            // interpolated in both axes
    float harmonic (float frame01, int h) const;                  // h = 0 is the fundamental
    const float* frame (int i) const { return frames.data() + (size_t) juce::jlimit (0, numFrames - 1, i) * kFrameLen; }

    // build from raw samples: frameLen > 0 slices frames, otherwise the
    // whole buffer is treated as one cycle and resampled
    static std::shared_ptr<Wavetable> fromSamples (const float* data, int length, int frameLen, const juce::String& name);
    static std::shared_ptr<Wavetable> builtIn();                  // sine > tri > saw > square > pulse > formant-ish
private:
    void analyse();
};

// ---------------------------------------------------------------
// Humanizer: a repeatable random value in -1..1 for (seed, index), shared by
// the DSP and the visual (Steps mode draws the exact per-step levels)
inline float humanizeHash (int seed, long long i)
{
    uint64_t x = (uint64_t) (i * 0x9E3779B97F4A7C15ull) ^ ((uint64_t) (seed + 1) * 0xBF58476D1CE4E5B9ull);
    x ^= x >> 31; x *= 0x94D049BB133111EBull; x ^= x >> 29;
    return (float) ((double) (x >> 11) / (double) (1ull << 53)) * 2.0f - 1.0f;
}

// ---------------------------------------------------------------
// Chord qualities the Chord Detector matches: intervals from the root (up to
// six notes, -1 = unused). Shared by the detector and its visual. Earlier
// entries win ties, so plainer chords come first; no two entries share a
// pitch-class set on the same root (the bass note picks between inversions).
struct ChordQuality { const char* name; int iv[6]; };
inline constexpr ChordQuality kChordQualities[] = {
    { "5", { 0, 7, -1, -1, -1, -1 } },
    { "maj", { 0, 4, 7, -1, -1, -1 } },
    { "min", { 0, 3, 7, -1, -1, -1 } },
    { "sus4", { 0, 5, 7, -1, -1, -1 } },
    { "sus2", { 0, 2, 7, -1, -1, -1 } },
    { "dim", { 0, 3, 6, -1, -1, -1 } },
    { "aug", { 0, 4, 8, -1, -1, -1 } },
    { "7", { 0, 4, 7, 10, -1, -1 } },
    { "maj7", { 0, 4, 7, 11, -1, -1 } },
    { "m7", { 0, 3, 7, 10, -1, -1 } },
    { "6", { 0, 4, 7, 9, -1, -1 } },
    { "m6", { 0, 3, 7, 9, -1, -1 } },
    { "add9", { 0, 4, 7, 2, -1, -1 } },
    { "madd9", { 0, 3, 7, 2, -1, -1 } },
    { "7sus4", { 0, 5, 7, 10, -1, -1 } },
    { "m7b5", { 0, 3, 6, 10, -1, -1 } },
    { "dim7", { 0, 3, 6, 9, -1, -1 } },
    { "mMaj7", { 0, 3, 7, 11, -1, -1 } },
    { "7#5", { 0, 4, 8, 10, -1, -1 } },
    { "maj7#5", { 0, 4, 8, 11, -1, -1 } },
    { "7b5", { 0, 4, 6, 10, -1, -1 } },
    { "maj7b5", { 0, 4, 6, 11, -1, -1 } },
    { "7sus2", { 0, 2, 7, 10, -1, -1 } },
    { "maj7sus4", { 0, 5, 7, 11, -1, -1 } },
    { "maj7sus2", { 0, 2, 7, 11, -1, -1 } },
    { "dimMaj7", { 0, 3, 6, 11, -1, -1 } },
    { "add11", { 0, 4, 7, 5, -1, -1 } },
    { "madd11", { 0, 3, 7, 5, -1, -1 } },
    { "add#11", { 0, 4, 7, 6, -1, -1 } },
    { "addb9", { 0, 4, 7, 1, -1, -1 } },
    { "sus4add9", { 0, 5, 7, 2, -1, -1 } },
    { "b5", { 0, 4, 6, -1, -1, -1 } },
    { "m(no5)", { 0, 3, -1, -1, -1, -1 } },
    { "(no5)", { 0, 4, -1, -1, -1, -1 } },
    { "6sus4", { 0, 5, 7, 9, -1, -1 } },
    { "m(b6)", { 0, 3, 7, 8, -1, -1 } },
    { "4ths", { 0, 5, 10, -1, -1, -1 } },
    { "7(no5)", { 0, 4, 10, -1, -1, -1 } },
    { "m7(no5)", { 0, 3, 10, -1, -1, -1 } },
    { "maj7(no5)", { 0, 4, 11, -1, -1, -1 } },
    { "6/9", { 0, 4, 7, 9, 2, -1 } },
    { "m6/9", { 0, 3, 7, 9, 2, -1 } },
    { "9", { 0, 4, 7, 10, 2, -1 } },
    { "maj9", { 0, 4, 7, 11, 2, -1 } },
    { "m9", { 0, 3, 7, 10, 2, -1 } },
    { "mMaj9", { 0, 3, 7, 11, 2, -1 } },
    { "9sus4", { 0, 5, 7, 10, 2, -1 } },
    { "7b9", { 0, 4, 7, 10, 1, -1 } },
    { "7#9", { 0, 4, 7, 10, 3, -1 } },
    { "7#11", { 0, 4, 7, 10, 6, -1 } },
    { "maj7#11", { 0, 4, 7, 11, 6, -1 } },
    { "7b13", { 0, 4, 7, 10, 8, -1 } },
    { "m7add11", { 0, 3, 7, 10, 5, -1 } },
    { "maj7add13", { 0, 4, 7, 11, 9, -1 } },
    { "7add13", { 0, 4, 7, 10, 9, -1 } },
    { "m9b5", { 0, 3, 6, 10, 2, -1 } },
    { "9b5", { 0, 4, 6, 10, 2, -1 } },
    { "9#5", { 0, 4, 8, 10, 2, -1 } },
    { "7#5#9", { 0, 4, 8, 10, 3, -1 } },
    { "7b5b9", { 0, 4, 6, 10, 1, -1 } },
    { "7#5b9", { 0, 4, 8, 10, 1, -1 } },
    { "7b5#9", { 0, 4, 6, 10, 3, -1 } },
    { "dim9", { 0, 3, 6, 9, 2, -1 } },
    { "m7b9", { 0, 3, 7, 10, 1, -1 } },
    { "maj9#5", { 0, 4, 8, 11, 2, -1 } },
    { "m11", { 0, 3, 7, 10, 2, 5 } },
    { "maj11", { 0, 4, 7, 11, 2, 5 } },
    { "13", { 0, 4, 7, 10, 2, 9 } },
    { "maj13", { 0, 4, 7, 11, 2, 9 } },
    { "m13", { 0, 3, 7, 10, 2, 9 } },
    { "9#11", { 0, 4, 7, 10, 2, 6 } },
    { "maj9#11", { 0, 4, 7, 11, 2, 6 } },
    { "13#11", { 0, 4, 7, 10, 6, 9 } },
    { "13b9", { 0, 4, 7, 10, 1, 9 } },
    { "13sus4", { 0, 5, 7, 10, 2, 9 } },
    { "7alt", { 0, 4, 10, 1, 3, 8 } },
    { "m11b5", { 0, 3, 6, 10, 2, 5 } },
    { "mMaj11", { 0, 3, 7, 11, 2, 5 } },
    { "Elektra", { 0, 7, 9, 1, 4, -1 } },
    { "Mystic", { 0, 6, 10, 4, 9, 2 } },
    { "Petrushka", { 0, 4, 7, 6, 10, 1 } },
};
inline constexpr int kNumChordQualities = (int) (sizeof (kChordQualities) / sizeof (kChordQualities[0]));

// ---------------------------------------------------------------
// Shape-data layouts shared by the DSP and the visual editors
namespace Shape
{
    // Multiband Gate: band b at b * kMbgStride: threshold dB (-80..0), range dB
    // (-80..0, the level a closed band drops by), on (1 = gated, 0 = passes)
    constexpr int kMbgBands = 5, kMbgStride = 3;
    constexpr float kMbgFloorDb = -80.0f;
    // crossover k (0..3) from the controls, kept in order and apart
    inline float mbgCrossover (const float* p, int k)
    {
        float f = 20.0f;
        for (int i = 0; i <= k; ++i) f = juce::jmax (p[i], f * 1.15f);
        return juce::jmin (f, 18000.0f);
    }
    // 8-Band EQ: band b at b * kEqStride: on, type, freq Hz, gain dB, Q
    constexpr int kEqBands = 8, kEqStride = 5;
    inline const char* const kEqTypes[] = { "Peak", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
    constexpr int kNumEqTypes = 7;
    // Multiband Dynamics: band b at b * 4: down thresh dB, up thresh dB, down ratio, up ratio
    // Trance Gate: 32 step levels at 0
    constexpr int kSteps = 32;
    // Additive Drive: harmonic 2..16 levels at 0 (15 values)
    constexpr int kHarmonics = 15;
    // Spectral Comp: 16 per-band threshold offsets (dB) at 0
    // Spectral Delay: band b at b * 4: time x, feedback, level dB, pan  (8 bands)
    constexpr int kSdBands = 8;
    // LFO / duck / custom-curve effects: CurvePoints at 0
}

// ---------------------------------------------------------------
// A user audio file attached to a slot (Convolution Verb impulse response).
struct AudioAsset
{
    juce::AudioBuffer<float> buf;   // 1 or 2 channels
    double sampleRate = 48000.0;
    juce::String name;
};

// ---------------------------------------------------------------
// everything an FxImpl needs for one sub-block
struct FxContext
{
    double sr = 44100.0;
    double bpm = 120.0;
    double ppq = 0.0;               // beat position at the first sample of this sub-block
    double beatsPerSample = 0.0;
    bool playing = false;
    int mode = 0;
    const float* p = nullptr;       // physical control values (macro + smoothing applied)
    const float* n = nullptr;       // the same, normalised 0..1
    const float* scL = nullptr;     // sidechain for this sub-block (nullptr = none)
    const float* scR = nullptr;
    const Wavetable* table = nullptr;
    const float* data = nullptr;    // the slot's shape data (kDataSize values)
    int dataGen = 0;                // changes whenever the data was edited
    double samplesFor (double beats) const { return beats * 60.0 / juce::jmax (1.0, bpm) * sr; }
};

// visual taps written by the audio thread, read by the editor
struct VizTaps
{
    static constexpr int kScope = 512;
    static constexpr int kLongScope = 8192;
    static constexpr int kSpec = 96;
    static constexpr int kMeters = 96;

    std::array<float, kScope> scopeMid {}, scopeSide {};
    std::atomic<int> scopeW { 0 };
    std::atomic<float> levelDb { -100.0f }, peakDb { -100.0f }, inLevelDb { -100.0f };
    // general-purpose meters (GR, band gains, LFO phase, ...); meaning is per effect
    std::array<std::atomic<float>, kMeters> m {};
    // analysis effects
    std::array<std::atomic<float>, kSpec> spec {}, specPeak {};
    std::vector<float> longScope;   // oscilloscope history (allocated for the scope only)
    std::atomic<int> longW { 0 };
    std::atomic<float> lufsM { -100.0f }, lufsS { -100.0f }, lufsI { -100.0f }, corr { 0.0f };
    std::atomic<float> pitchHz { 0.0f }, pitchConf { 0.0f };

    void set (int i, float v) { if (i >= 0 && i < kMeters) m[(size_t) i].store (v, std::memory_order_relaxed); }
    float get (int i) const { return (i >= 0 && i < kMeters) ? m[(size_t) i].load (std::memory_order_relaxed) : 0.0f; }
};

// ---------------------------------------------------------------
class FxImpl
{
public:
    virtual ~FxImpl() = default;
    virtual void prepare (double sampleRate) = 0;           // allocate everything here
    virtual void reset() = 0;
    virtual void process (float* L, float* R, int n, const FxContext& c) = 0;   // n <= kChunk
    // the effect sequencer turned this slot on: a run of runBeats starts,
    // phaseBeats of it already gone (a host jump into the middle). Time
    // effects (stutter, reverse, tape stop ...) restart from it; others ignore it.
    virtual void trigger (double /*runBeats*/, double /*phaseBeats*/, double /*bpm*/) {}
    // message-thread hook (~4 Hz) for work that may allocate (e.g. IR rebuilds)
    virtual void idle (const float* /*phys*/, int /*mode*/) {}
    // audio-free description for the visualisers, evaluated on the UI thread
    // from physical parameter values and the slot's shape data
    virtual bool transfer (float /*x*/, const float* /*p*/, int /*mode*/, const float* /*data*/, float& /*y*/) const { return false; }
    virtual bool responseDb (float /*hz*/, const float* /*p*/, int /*mode*/, const float* /*data*/, double /*sr*/, float& /*db*/) const { return false; }
    // shape data: effects that use it fill their defaults here (message thread)
    virtual bool usesData() const { return false; }
    virtual void defaultData (float* /*d*/) const {}

    VizTaps* taps = nullptr;
    const EffectDef* def = nullptr;
    const std::atomic<const Wavetable*>* tableRef = nullptr;   // the slot's table (wavetable effects)
    const std::atomic<const AudioAsset*>* assetRef = nullptr;  // the slot's user audio (convolution IR)
    double sr = 44100.0;
    const Wavetable* currentTable() const
    {
        const Wavetable* t = tableRef != nullptr ? tableRef->load (std::memory_order_acquire) : nullptr;
        return t != nullptr ? t : Wavetable::builtIn().get();
    }
};

std::unique_ptr<FxImpl> makeFxImpl (int effectId);   // defined in fx/Factory.cpp

// ---------------------------------------------------------------
// per-block input from the processor
struct SlotInput
{
    const float* norm = nullptr;        // kMaxParams normalised knob values
    const float* macroDepth = nullptr;  // kMaxParams user macro routing (-1..1)
    float macro = 0.0f;                 // 0..1
    float mix = 1.0f;
    int mode = 0;
    bool bypass = false;
    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = false;
    const float* scL = nullptr;         // whole-block sidechain (nullptr = none)
    const float* scR = nullptr;
    const float* data = nullptr;        // kDataSize shape values (nullptr = effect defaults)
    int dataGen = 0;
    // effect sequencer: gate changes inside this block (sample offsets ascending).
    // The slot follows them; without events it stays as it was (open by default).
    struct GateEvent { int offset = 0; bool on = false; double runBeats = 0.0, phaseBeats = 0.0; };
    const GateEvent* gateEvents = nullptr;
    int numGateEvents = 0;
    bool gateRing = false;              // Ring: gate the input, tails ring on; Cut: gate the wet signal
    float gateFadeMs = 3.0f;
};

class GridEffect
{
public:
    static constexpr int kChunk = 32;

    explicit GridEffect (int effectId);
    ~GridEffect();

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (float* L, float* R, int numSamples, const SlotInput& in);
    void idle();                                       // message thread

    int getId() const { return id; }
    bool gateOpen() const { return gateTo > 0.5f; }   // the sequencer has this slot on (UI light)
    const EffectDef& def() const { return *d; }
    double sampleRate() const { return sr; }

    // wavetable (message thread). The previous table must outlive the audio
    // thread's use of it — the processor keeps old tables alive briefly.
    void setWavetable (const Wavetable* t) { table.store (t, std::memory_order_release); }
    const Wavetable* wavetable() const { return table.load (std::memory_order_acquire); }
    void setAsset (const AudioAsset* a) { asset.store (a, std::memory_order_release); }
    const AudioAsset* audioAsset() const { return asset.load (std::memory_order_acquire); }
    bool usesData() const { return impl->usesData(); }
    void defaultData (float* d) const { impl->defaultData (d); }

    // ---- editor-facing state ----
    VizTaps& taps() { return vt; }
    const VizTaps& taps() const { return vt; }
    float effectiveNorm (int k) const { return uiNorm[(size_t) juce::jlimit (0, kMaxParams - 1, k)].load (std::memory_order_relaxed); }
    float physical (int k) const { return uiPhys[(size_t) juce::jlimit (0, kMaxParams - 1, k)].load (std::memory_order_relaxed); }
    void physicalAll (float* out) const { for (int k = 0; k < kMaxParams; ++k) out[k] = physical (k); }
    int mode() const { return uiMode.load (std::memory_order_relaxed); }
    double ppq() const { return uiPpq.load (std::memory_order_relaxed); }
    double bpm() const { return uiBpm.load (std::memory_order_relaxed); }
    float levelDb() const { return vt.levelDb.load(); }
    float peakDb() const { return vt.peakDb.load(); }
    // visual probes (UI thread): physical params, mode and shape data supplied by the caller
    bool transfer (float x, const float* phys, int mode, const float* data, float& y) const { return impl->transfer (x, phys, mode, data, y); }
    bool responseDb (float hz, const float* phys, int mode, const float* data, float& db) const { return impl->responseDb (hz, phys, mode, data, sr, db); }

private:
    void pushScope (const float* L, const float* R, int n, const float* inL, const float* inR);

    int id = 0;
    const EffectDef* d = nullptr;
    std::unique_ptr<FxImpl> impl;
    double sr = 44100.0;
    bool primed = false;
    float smoothN[kMaxParams] {}, phys[kMaxParams] {};
    float mixS = 1.0f, activeS = 1.0f;
    // effect sequencer gate: raised-cosine ramp from gateFrom to gateTo
    float gateS = 1.0f, gateFrom = 1.0f, gateTo = 1.0f;
    int gatePos = 0, gateLen = 0;
    float gateBuf[kChunk] {};
    float smoothCoef = 0.2f;
    double freePpq = 0.0;
    float dryL[kChunk] {}, dryR[kChunk] {};
    float data[kDataSize] {};
    std::atomic<const Wavetable*> table { nullptr };
    std::atomic<const AudioAsset*> asset { nullptr };

    VizTaps vt;
    std::array<std::atomic<float>, kMaxParams> uiNorm {}, uiPhys {};
    std::atomic<int> uiMode { 0 };
    std::atomic<double> uiPpq { 0.0 }, uiBpm { 120.0 };
};

// factory: 0 -> nullptr, 1..kNumEffects -> new GridEffect(id)
std::unique_ptr<GridEffect> createEffect (int id);

} // namespace efx
