#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "Visuals.h"

// The UI-side state of a slot for drawing (knob values with the macro
// applied, mode, shape data, wavetable / IR).
viz::State buildVizState (EfxGridProcessor& proc, int slot, bool detailed,
                          std::shared_ptr<const efx::Wavetable>& tableHolder);

// ============================================================
// The expanded panel's visualiser, made editable: drag nodes and handles to
// set parameters (EQ / filter nodes, thresholds, envelope points, delay
// taps, crossovers, reverb decay ...), draw curves / steps / bars into the
// slot's shape data. Scroll adjusts Q / ratio / level where that applies;
// right-click offers types and presets.
// ============================================================
class VizEditor : public juce::Component
{
public:
    explicit VizEditor (EfxGridProcessor& p) : proc (p) {}
    void setSlot (int s) { slot = s; focusBand = 1; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    std::function<void()> onEdited;
    juce::String hint() const;                // how to edit this visual (empty = view only)
    std::vector<juce::Point<float>> handlePoints() const;   // draggable nodes (for tests)

private:
    struct Handle { juce::Point<float> pos; int id = -1; };
    enum class Drag { None, Handle, Curve, Steps, Bars, Free, Keys };

    viz::State state (std::shared_ptr<const efx::Wavetable>& holder) const
    {
        auto s = buildVizState (proc, slot, true, holder);
        s.spanMs = frozenSpan;   // keep the time axis still while dragging
        return s;
    }
    juce::Rectangle<float> plot() const { return viz::plotArea (getLocalBounds().toFloat(), true); }
    std::vector<Handle> handles (const viz::State& s) const;
    int hitHandle (const viz::State& s, juce::Point<float> p) const;

    // parameter access (raw knob values, without the macro)
    float phys (int k) const;
    float norm (int k) const;
    void setPhys (int k, float v);
    void setNorm (int k, float n);
    void beginGesture (std::initializer_list<int> ks);
    void endGestures();
    float data (int i) const { return proc.getSlotData (slot, i); }
    void setData (int i, float v) { proc.setSlotData (slot, i, v); }
    const efx::EffectDef* def() const { return efx::defForId (proc.getSlotEffectId (slot)); }

    // per-kind editing
    void dragHandle (int id, const juce::MouseEvent& e);
    void wheelHandle (int id, float delta);
    void handleMenu (int id);
    void backgroundMenu();
    // curve editing (shape data CurvePoints)
    juce::Rectangle<float> curveArea() const;
    float curveYToValue (float y) const;
    float curveValueToY (float v) const;
    bool curveBipolar() const;
    int hitCurvePoint (juce::Point<float> p) const;
    void ensureCustomCurve();
    void ensureCustomSet();                  // Harmonic Resonator keyboard edits
    void toggleMbgBand (int b);              // Multiband Gate band on / off
    void beginBend (juce::Point<float> p);   // drag between points: bend that segment
    void setStepOrBar (juce::Point<float> p);
    void edited() { repaint(); if (onEdited) onEdited(); }

    EfxGridProcessor& proc;
    int slot = 0;
    Drag drag = Drag::None;
    int dragId = -1, dragPoint = -1;
    juce::Point<float> downPos;
    float startVals[8] {};
    float msPerPx = 1.0f;
    float frozenSpan = 0.0f;
    std::vector<juce::RangedAudioParameter*> gestures;
    int hover = -1;
    int focusBand = 1;                       // EQ band showing its Q brackets
    static constexpr int kQHandle = 100;
    bool eqBand (const viz::State& s, int b, float& hz, float& q) const;
};
