#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "dsp/Effects.h"

// ============================================================
// Per-effect visualisers. Each one is drawn from the slot's state as the UI
// sees it (knob values with the macro applied, mode, shape data), plus the
// live meters the effect publishes, so edits redraw immediately even when no
// audio is running.
// ============================================================

namespace viz
{
    struct State
    {
        const efx::GridEffect* fx = nullptr;     // live instance (meters), may be null
        const efx::EffectDef* d = nullptr;
        int fxId = 0;
        float p[efx::kMaxParams] {};             // physical values (macro applied)
        float norm[efx::kMaxParams] {};          // normalised values (macro applied)
        int mode = 0;
        float data[efx::kDataSize] {};
        const efx::Wavetable* table = nullptr;
        juce::String assetName;                  // user IR name, if any
        juce::Colour accent;
        bool detailed = false;
        double sr = 48000.0;
        bool bypassed = false;
        float spanMs = 0.0f;                     // time axis held by the editor while dragging (0 = auto)

        float P (int k) const { return p[juce::jlimit (0, efx::kMaxParams - 1, k)]; }
        int I (int k) const { return (int) std::lround (P (k)); }
        float m (int i) const { return fx != nullptr ? fx->taps().get (i) : 0.0f; }
    };

    // Draws the visual filling `r` (rounded, clipped).
    void drawEffectVisual (juce::Graphics& g, juce::Rectangle<float> r, const State& s);

    // Short live readout for the panel (e.g. "GR -4.2 dB", "A4 +3 ct").
    juce::String readoutTag (const State& s);

    // ---- geometry shared with the interactive editor (VizEditor) ----
    constexpr float kFLo = 20.0f, kFHi = 20000.0f;
    juce::Rectangle<float> plotArea (juce::Rectangle<float> full, bool detailed);
    inline float xOfHz (juce::Rectangle<float> r, float hz) { return r.getX() + r.getWidth() * std::log (juce::jlimit (kFLo, kFHi, hz) / kFLo) / std::log (kFHi / kFLo); }
    inline float hzOfX (juce::Rectangle<float> r, float x) { return kFLo * std::pow (kFHi / kFLo, juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth())); }
    inline float yOfDb (juce::Rectangle<float> r, float db, float lo, float hi) { return r.getBottom() - (juce::jlimit (lo, hi, db) - lo) / (hi - lo) * r.getHeight(); }
    inline float dbOfY (juce::Rectangle<float> r, float y, float lo, float hi) { return lo + (r.getBottom() - y) / r.getHeight() * (hi - lo); }
    // dB range of the response display for an EQ / filter visual
    void responseRange (efx::Viz v, float& lo, float& hi);
    // transfer display range (dB) for dynamics
    constexpr float kDynLo = -60.0f, kDynHi = 6.0f;
    // Transient Shaper envelope display: time axis, and the gain change (dB)
    // drawn for a single hit at time ms (attack, decay, sustain, release)
    constexpr float kTsRangeDb = 18.0f;
    float transientSpanMs (const State& s);
    float transientCurveDb (const State& s, float ms);
    // Harmonic Resonator: response curve area and the one-octave keyboard
    // under it; key rectangle / key under a point (pitch class 0..11, -1 none)
    juce::Rectangle<float> harmResCurve (juce::Rectangle<float> plot, bool detailed);
    juce::Rectangle<float> harmResKeys (juce::Rectangle<float> plot, bool detailed);
    juce::Rectangle<float> harmResKeyRect (juce::Rectangle<float> keys, int pc);
    int harmResKeyAt (juce::Rectangle<float> keys, juce::Point<float> p);
    // Multiband Gate: level axis (dB) and band b's x span from the crossovers
    constexpr float kMbgLo = -80.0f, kMbgHi = 0.0f;
    juce::Range<float> mbgBandX (juce::Rectangle<float> plot, const State& s, int b);
    // LFO / curve shape displays show one cycle in the panel, two on a tile
    inline int lfoCycles (bool detailed) { return detailed ? 1 : 2; }
    // vertical scale a drawn curve uses (LFO depth, ducker depth ...); the
    // editor maps the mouse with the same scale so points land under it
    float curveScale (const State& s);
    // which shape-data curve range: true = bipolar (-1..1), false = 0..1
    bool curveBipolar (efx::Viz v);
    bool isLfoViz (efx::Viz v);
    // reverb / delay visuals drawn on a time axis
    inline bool isTimeViz (efx::Viz v)
    {
        using namespace efx;
        switch (v) { case VizRoom: case VizPlate: case VizShimmer: case VizAlgo: case VizConv: case VizSpring:
                     case VizSimpleDly: case VizPingPong: case VizEcho: case VizPrecDly: case VizWarp: return true;
                     default: return false; }
    }
    // shape control index for the LFO effects (-1 if none), and its morph control
    int lfoShapeIndex (const efx::EffectDef& d);
    int lfoMorphIndex (const efx::EffectDef& d);
    // delay tap layout as drawn (times ms, gains 0..1, right channel flags)
    constexpr int kMaxTaps = 24;
    int delayTaps (const State& s, float* times, float* gains, bool* right);
    // delay time (ms) from the controls: free time, or the synced division at
    // the host tempo (120 bpm when no audio has run yet)
    float delayBaseMs (const State& s, float* msR = nullptr);
    float divisionBeats (int index);
    float delayBpm (const State& s);
    // time axes: fixed, "nice" spans so handles follow the mouse
    float niceSpanMs (float needMs);
    float delaySpanMs (const State& s);
    float decaySpanMs (const State& s);
    inline float xOfMs (juce::Rectangle<float> r, float ms, float span) { return r.getX() + ms / span * r.getWidth(); }
    inline float msOfX (juce::Rectangle<float> r, float x, float span) { return juce::jmax (0.0f, (x - r.getX()) / r.getWidth() * span); }
    // tap k as drawn (top of its bar)
    juce::Point<float> delayTapPos (juce::Rectangle<float> plot, const State& s, int k);
    inline juce::Point<float> delayFirstTap (juce::Rectangle<float> plot, const State& s) { return delayTapPos (plot, s, 0); }
    // reverb decay drawn in dB: 0 dB at the top of the curve, -60 dB (RT60) on the floor
    inline float decayYOfDb (juce::Rectangle<float> r, float db) { return r.getBottom() - 2.0f - (1.0f + juce::jlimit (-60.0f, 0.0f, db) / 60.0f) * r.getHeight() * 0.85f; }
    // reverb decay display: the RT60 drawn, and which controls set decay / pre-delay
    float decayShownRt (const State& s, int& iDecay, int& iPre);
    const char* chordQualityName (int q);
    juce::String noteName (int midi, bool flats = false);
}
