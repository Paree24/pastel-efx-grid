#include "VizEditor.h"
#include "dsp/HarmRes.h"

using namespace efx;

viz::State buildVizState (EfxGridProcessor& proc, int slot, bool detailed, std::shared_ptr<const Wavetable>& tableHolder)
{
    viz::State s;
    s.fxId = proc.getSlotEffectId (slot);
    s.d = defForId (s.fxId);
    s.detailed = detailed;
    auto* fx = proc.getEffect (slot);
    s.fx = (fx != nullptr && fx->getId() == s.fxId) ? fx : nullptr;
    if (s.d == nullptr) return s;
    s.accent = catAccent ((int) s.d->cat);
    s.mode = proc.getSlotMode (slot);
    const float macro = proc.apvts.getRawParameterValue (EfxGridProcessor::paramId (slot, "macro"))->load();
    for (int k = 0; k < kMaxParams; ++k)
    {
        float n = proc.getNorm (slot, k);
        if (! s.d->p[k].isChoice()) n = clamp01 (n + macro * proc.getMacroDepth (slot, k));
        s.norm[k] = n;
        s.p[k] = unmapParam (s.d->p[k], n);
    }
    proc.getSlotDataAll (slot, s.data);
    if (s.d->usesWavetable()) { tableHolder = proc.getWavetable (slot); s.table = tableHolder.get(); }
    if (auto a = proc.getAudioAsset (slot)) s.assetName = a->name;
    s.sr = s.fx != nullptr ? s.fx->sampleRate() : 48000.0;
    s.bypassed = proc.apvts.getRawParameterValue (EfxGridProcessor::paramId (slot, "bypass"))->load() > 0.5f;
    return s;
}

// ------------------------------------------------------------------ parameters
float VizEditor::norm (int k) const { return proc.getNorm (slot, k); }
float VizEditor::phys (int k) const { const auto* d = def(); return d != nullptr ? unmapParam (d->p[k], norm (k)) : 0.0f; }

void VizEditor::setNorm (int k, float n)
{
    if (auto* p = proc.apvts.getParameter (EfxGridProcessor::paramId (slot, "p" + juce::String (k))))
        p->setValueNotifyingHost (clamp01 (n));
}

void VizEditor::setPhys (int k, float v)
{
    if (const auto* d = def()) setNorm (k, mapParam (d->p[k], v));
}

void VizEditor::beginGesture (std::initializer_list<int> ks)
{
    for (int k : ks)
        if (auto* p = proc.apvts.getParameter (EfxGridProcessor::paramId (slot, "p" + juce::String (k))))
        {
            p->beginChangeGesture();
            gestures.push_back (p);
        }
}

void VizEditor::endGestures()
{
    for (auto* p : gestures) p->endChangeGesture();
    gestures.clear();
}

// ------------------------------------------------------------------ geometry
juce::Rectangle<float> VizEditor::curveArea() const
{
    const auto* d = def();
    auto r = plot();
    if (d != nullptr && d->viz == VizWaveShaper) r = r.withTrimmedLeft (r.getWidth() * 0.5f);
    return r;
}

bool VizEditor::curveBipolar() const
{
    const auto* d = def();
    return d != nullptr && (viz::isLfoViz (d->viz) || d->viz == VizWaveShaper);
}

float VizEditor::curveValueToY (float v) const
{
    std::shared_ptr<const Wavetable> h;
    const auto s = state (h);
    const auto r = plot();
    if (s.d->viz == VizWaveShaper) return r.getCentreY() - v * r.getHeight() * 0.45f;
    if (s.d->viz == VizDuck) return r.getY() + 3.0f + clamp01 (v * viz::curveScale (s)) * (r.getHeight() - 6.0f);
    return r.getCentreY() - v * viz::curveScale (s) * r.getHeight() * 0.44f;
}

float VizEditor::curveYToValue (float y) const
{
    std::shared_ptr<const Wavetable> h;
    const auto s = state (h);
    const auto r = plot();
    if (s.d->viz == VizWaveShaper) return juce::jlimit (-1.0f, 1.2f, (r.getCentreY() - y) / (r.getHeight() * 0.45f));
    if (s.d->viz == VizDuck) return clamp01 ((y - r.getY() - 3.0f) / ((r.getHeight() - 6.0f) * viz::curveScale (s)));
    return juce::jlimit (-1.0f, 1.0f, (r.getCentreY() - y) / (viz::curveScale (s) * r.getHeight() * 0.44f));
}

int VizEditor::hitCurvePoint (juce::Point<float> p) const
{
    const auto r = curveArea();
    float buf[kDataSize];
    proc.getSlotDataAll (slot, buf);
    const int cnt = CurvePoints::count (buf);
    for (int i = 0; i < cnt; ++i)
    {
        const juce::Point<float> q (r.getX() + buf[1 + i * 3] * r.getWidth(), curveValueToY (buf[2 + i * 3]));
        if (q.getDistanceFrom (p) < 9.0f) return i;
    }
    return -1;
}

// switch an LFO / waveshaper to its Custom shape, seeding the points from what is shown
void VizEditor::ensureCustomSet()
{
    // Harmonic Resonator: switching to Custom starts from the set that was playing
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    if (s.mode == harmres::kModeCustom) return;
    float w12[12];
    harmres::pitchSet (s.p, s.mode, s.data, w12);
    proc.setSlotDataBlock (slot, 0, w12, 12);
    proc.setSlotMode (slot, harmres::kModeCustom);
}

void VizEditor::ensureCustomCurve()
{
    const auto* d = def();
    if (d == nullptr) return;
    if (viz::isLfoViz (d->viz))
    {
        const int si = viz::lfoShapeIndex (*d), mi = viz::lfoMorphIndex (*d);
        if (si < 0 || (int) std::lround (phys (si)) == LfoShapes::kCustom) return;
        const int shape = (int) std::lround (phys (si));
        const float morph = mi >= 0 ? phys (mi) : 0.0f;
        float buf[kDataSize];
        proc.getSlotDataAll (slot, buf);
        float pts[CurvePoints::kSize] {};
        ShapeLfo l; l.held = 0.6f; l.next = -0.4f;
        const int n = (shape == 4 || shape == 5 || shape == 6) ? 9 : 9;
        for (int i = 0; i < n; ++i)
        {
            const float x = (float) i / (float) (n - 1);
            pts[1 + i * 3] = x;
            pts[2 + i * 3] = l.value (shape, morph, buf, juce::jmin (0.9999, (double) x));
            pts[3 + i * 3] = 0.0f;
        }
        pts[0] = (float) n;
        proc.setSlotDataBlock (slot, 0, pts, CurvePoints::kSize);
        setNorm (si, mapParam (d->p[si], (float) LfoShapes::kCustom));
        if (mi >= 0) setNorm (mi, 0.0f);
    }
    else if (d->viz == VizWaveShaper && (int) std::lround (phys (0)) != 11)
        setNorm (0, mapParam (d->p[0], 11.0f));
}

// ------------------------------------------------------------------ handles
// Q <-> half the bandwidth in octaves (EQ Q brackets)
static float qToHalfOct (float q) { return std::asinh (1.0f / (2.0f * juce::jmax (0.05f, q))) / std::log (2.0f); }
static float halfOctToQ (float h) { return 1.0f / (2.0f * std::sinh (juce::jmax (0.01f, h) * std::log (2.0f))); }

std::vector<VizEditor::Handle> VizEditor::handles (const viz::State& s) const
{
    std::vector<Handle> hs;
    if (s.d == nullptr) return hs;
    const auto r = plot();
    float lo, hi;
    viz::responseRange (s.d->viz, lo, hi);
    auto resp = [&] (float hz) { float db = 0.0f; if (s.fx != nullptr) s.fx->responseDb (hz, s.p, s.mode, s.data, db); return db; };
    switch (s.d->viz)
    {
        case VizEq3:
            for (int b = 0; b < 3; ++b)
            {
                const int type = s.I (8 + b);
                const bool noGain = (b == 0 && type == 0) || (b == 2 && type == 1) || (b == 1 && type != 0);
                const float f = s.P (3 + b);
                hs.push_back ({ { viz::xOfHz (r, f), noGain ? viz::yOfDb (r, resp (f), lo, hi) : viz::yOfDb (r, s.P (b), lo, hi) }, b });
            }
            break;
        case VizEq8:
            for (int b = 0; b < Shape::kEqBands; ++b)
            {
                const float* x = s.data + b * Shape::kEqStride;
                const int type = (int) std::lround (x[1]);
                hs.push_back ({ { viz::xOfHz (r, x[2]), type <= 2 ? viz::yOfDb (r, x[3] * s.P (1), lo, hi) : viz::yOfDb (r, 0.0f, lo, hi) }, b });
            }
            break;
        case VizMultimode: case VizSvf: case VizLadder: case VizVintage: case VizComb: case VizDeess:
            hs.push_back ({ { viz::xOfHz (r, s.P (0)), viz::yOfDb (r, resp (s.P (0)), lo, hi) }, 0 });
            break;
        case VizAllpass:
            hs.push_back ({ { viz::xOfHz (r, s.P (0)), r.getCentreY() }, 0 });
            break;
        case VizComp: case VizSidechain: case VizGate: case VizExpander:
        {
            const float th = s.P (0);
            float y = th;
            if (s.fx != nullptr) s.fx->transfer (th, s.p, s.mode, s.data, y);
            hs.push_back ({ { r.getX() + (th - viz::kDynLo) / (viz::kDynHi - viz::kDynLo) * r.getWidth(), viz::yOfDb (r, y, viz::kDynLo, viz::kDynHi) }, 0 });
            break;
        }
        case VizLimit:
        {
            const float th = s.P (1);
            hs.push_back ({ { r.getX() + (th - viz::kDynLo) / (viz::kDynHi - viz::kDynLo) * r.getWidth(), viz::yOfDb (r, th + s.P (5), viz::kDynLo, viz::kDynHi) }, 0 });
            break;
        }
        case VizOtt:
        {
            const float cw = r.getWidth() / 3.0f;
            for (int b = 0; b < 3; ++b)
            {
                auto col = juce::Rectangle<float> (r.getX() + cw * (float) b, r.getY(), cw, r.getHeight()).reduced (14.0f, 2.0f);
                col.removeFromBottom (14.0f);
                for (int k = 0; k < 2; ++k)
                    hs.push_back ({ { col.getRight() - 10.0f, viz::yOfDb (col, s.data[b * 4 + k], -80.0f, 0.0f) }, b * 2 + k });
            }
            break;
        }
        case VizTransient:
        {
            // attack bump (Attack), decay point (Decay), sustain level (Sustain), tail end (Release)
            auto tr = r.withTrimmedRight (10.0f);
            const float span = viz::transientSpanMs (s), dec = s.P (6), rel = s.P (5);
            auto X = [&] (float ms) { return viz::xOfMs (tr, ms, span); };
            auto Y = [&] (float db) { return viz::yOfDb (tr, db, -viz::kTsRangeDb, viz::kTsRangeDb); };
            hs.push_back ({ { X (span * 0.01f), Y (s.P (0) * 12.0f) }, 0 });
            hs.push_back ({ { X (dec), Y (viz::transientCurveDb (s, dec)) }, 1 });
            hs.push_back ({ { X (dec + rel * 0.5f), Y (s.P (1) * 12.0f) }, 2 });
            hs.push_back ({ { X (dec + rel), Y (s.P (1) * 12.0f) }, 3 });
            break;
        }
        case VizMbGate:
        {
            // band thresholds (0..4, bands that are on), crossovers (10..13)
            for (int b = 0; b < Shape::kMbgBands; ++b)
                if (s.data[b * Shape::kMbgStride + 2] > 0.5f)
                    hs.push_back ({ { viz::mbgBandX (r, s, b).getStart() + viz::mbgBandX (r, s, b).getLength() * 0.5f,
                                      viz::yOfDb (r, s.data[b * Shape::kMbgStride] + s.P (4), viz::kMbgLo, viz::kMbgHi) }, b });
            for (int k = 0; k < Shape::kMbgBands - 1; ++k)
                hs.push_back ({ { viz::xOfHz (r, Shape::mbgCrossover (s.p, k)), r.getY() + 10.0f }, 10 + k });
            break;
        }
        case VizHarmRes:
        {
            auto cr = viz::harmResCurve (r, true);
            hs.push_back ({ { viz::xOfHz (cr, s.P (3)), cr.getBottom() - 10.0f }, 0 });
            hs.push_back ({ { viz::xOfHz (cr, s.P (4)), cr.getBottom() - 10.0f }, 1 });
            break;
        }
        case VizMbDrive:
            hs.push_back ({ { viz::xOfHz (r, s.P (3)), r.getBottom() - 14.0f }, 0 });
            hs.push_back ({ { viz::xOfHz (r, juce::jmax (s.P (4), s.P (3) * 1.5f)), r.getBottom() - 14.0f }, 1 });
            break;
        case VizMonoBass:
            hs.push_back ({ { viz::xOfHz (r.withTrimmedBottom (18.0f), s.P (0)), r.getY() + 12.0f }, 0 });
            break;
        case VizResSupp:
        {
            const float flo = 60.0f, fhi = 14000.0f;
            auto xOf = [&] (float f) { return r.getX() + r.getWidth() * std::log (juce::jlimit (flo, fhi, f) / flo) / std::log (fhi / flo); };
            hs.push_back ({ { xOf (s.P (2)), r.getBottom() - 12.0f }, 0 });
            hs.push_back ({ { xOf (s.P (3)), r.getBottom() - 12.0f }, 1 });
            break;
        }
        case VizSpecDly:
        {
            const int nb = s.I (6) == 0 ? 4 : (s.I (6) == 1 ? 6 : 8);
            for (int b = 0; b < nb; ++b)
            {
                const float y = r.getY() + r.getHeight() * ((float) (nb - 1 - b) + 0.5f) / (float) nb;
                const float ms = s.P (0) * juce::jlimit (0.05f, 8.0f, s.data[b * 4]) * std::pow (2.0f, s.P (1) * 2.0f * ((float) b / (float) juce::jmax (1, nb - 1) - 0.5f));
                hs.push_back ({ { r.getX() + clamp01 (std::log (1.0f + ms) / std::log (1.0f + 4000.0f)) * r.getWidth(), y }, b });
            }
            break;
        }
        case VizRoom: case VizPlate: case VizShimmer: case VizAlgo: case VizConv: case VizSpring:
        {
            int iDecay = 0, iPre = -1;
            const float rt = viz::decayShownRt (s, iDecay, iPre), pre = iPre >= 0 ? s.P (iPre) : 0.0f;
            const float span = viz::decaySpanMs (s);
            if (iPre >= 0) hs.push_back ({ { viz::xOfMs (r, pre, span), viz::decayYOfDb (r, 0.0f) }, 0 });
            hs.push_back ({ { juce::jmin (viz::xOfMs (r, pre + juce::jmax (50.0f, rt * 1000.0f), span), r.getRight() - 4.0f), r.getBottom() - 4.0f }, 1 });
            break;
        }
        case VizSimpleDly: case VizPingPong: case VizEcho: case VizPrecDly: case VizWarp:
            hs.push_back ({ viz::delayTapPos (r, s, 0), 0 });   // time
            hs.push_back ({ viz::delayTapPos (r, s, 1), 1 });   // feedback (height of the next repeat)
            break;
        case VizHaas:
        {
            auto area = r.withTrimmedBottom (18.0f);
            const float laneH = area.getHeight() / 2.0f;
            const int lane = s.mode == 0 ? 1 : 0;
            auto ln = juce::Rectangle<float> (area.getX(), area.getY() + laneH * (float) lane, area.getWidth(), laneH).reduced (2.0f, 6.0f);
            const float x = ln.getX() + 22.0f + juce::jmin (1.0f, s.P (0) / 45.0f) * (ln.getWidth() - 30.0f);
            hs.push_back ({ { x + 1.5f, ln.getCentreY() }, 0 });
            break;
        }
        case VizWah:
            if (s.mode == 2)
            {
                const float fc = s.P (3) * std::pow (s.P (4) / s.P (3), s.P (0));
                hs.push_back ({ { viz::xOfHz (r, fc), r.getY() + 12.0f }, 0 });
            }
            if (s.mode != 1)    // sweep range (Low / High)
            {
                hs.push_back ({ { viz::xOfHz (r, s.P (3)), r.getBottom() - 12.0f }, 1 });
                hs.push_back ({ { viz::xOfHz (r, s.P (4)), r.getBottom() - 12.0f }, 2 });
            }
            break;
        default: break;
    }
    // Q brackets around the focused EQ band
    float fq = 0.0f, q = 0.0f;
    if ((s.d->viz == VizEq3 || s.d->viz == VizEq8) && focusBand >= 0 && focusBand < (int) hs.size() && eqBand (s, focusBand, fq, q))
    {
        const auto c = hs[(size_t) focusBand].pos;
        const float h = qToHalfOct (q);
        const float xl = juce::jmin (viz::xOfHz (r, fq / std::pow (2.0f, h)), c.x - 14.0f);
        const float xr = juce::jmax (viz::xOfHz (r, fq * std::pow (2.0f, h)), c.x + 14.0f);
        hs.push_back ({ { xl, c.y }, kQHandle });
        hs.push_back ({ { xr, c.y }, kQHandle + 1 });
    }
    return hs;
}

std::vector<juce::Point<float>> VizEditor::handlePoints() const
{
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    std::vector<juce::Point<float>> pts;
    for (const auto& h : handles (s)) pts.push_back (h.pos);
    return pts;
}

int VizEditor::hitHandle (const viz::State& s, juce::Point<float> p) const
{
    int best = -1;
    float bestD = 12.0f;
    for (const auto& h : handles (s))
    {
        const float d = h.pos.getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = h.id; }
    }
    return best;
}

// EQ Q handles: ids kQHandle + 0 (left) / + 1 (right) bracket the focused band.
// Q <-> half the bandwidth in octaves.

bool VizEditor::eqBand (const viz::State& s, int b, float& hz, float& q) const
{
    if (s.d->viz == VizEq3)
    {
        if (b < 0 || b > 2) return false;
        hz = s.P (3 + b); q = s.P (b == 0 ? 11 : (b == 1 ? 6 : 12));
        return true;
    }
    if (s.d->viz == VizEq8)
    {
        if (b < 0 || b >= Shape::kEqBands) return false;
        const float* x = s.data + b * Shape::kEqStride;
        if (x[0] < 0.5f) return false;
        hz = x[2]; q = x[4];
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ paint
juce::String VizEditor::hint() const
{
    const auto* d = def();
    if (d == nullptr) return {};
    switch (d->viz)
    {
        case VizEq3:      return "Drag nodes: freq / gain. Drag the Q brackets (or scroll) for width. Right-click: band type";
        case VizEq8:      return "Drag nodes: freq / gain. Drag the Q brackets (or scroll) for width. Right-click: type. Double-click: add / toggle band";
        case VizMultimode: case VizSvf: case VizLadder: case VizVintage: return "Drag the node: cutoff / resonance. Scroll: resonance";
        case VizComb:     return "Drag the node: frequency / feedback";
        case VizDeess:    return "Drag the node: frequency";
        case VizAllpass:  return "Drag the node: frequency / Q";
        case VizComp: case VizSidechain: case VizExpander: return "Drag the knee: threshold / ratio. Scroll: ratio";
        case VizGate:     return "Drag the knee: threshold";
        case VizLimit:    return "Drag: ceiling / output";
        case VizOtt:      return "Drag the bars' edges: down / up thresholds. Scroll: ratio";
        case VizTransient: return "Drag the envelope: attack bump up / down, decay point sideways, sustain level, release end";
        case VizHarmRes:   return "Click keys to build your own set (switches to Custom), drag on a key for its level. Drag Low / High";
        case VizMbGate:    return "Drag a band's line: threshold; scroll: its range. Drag the top handles: crossovers. Double- / right-click a band: on / off";
        case VizDuck:     return "Drag / double-click points. Alt-drag bends. Right-click: presets";
        case VizTrance:   return "Click / drag to draw steps. Right-click: patterns";
        case VizAdditive: return "Draw harmonic levels. Right-click: presets";
        case VizSpecComp: return "Draw per-band threshold offsets. Right-click: presets";
        case VizWaveShaper: return "Draw on the curve (right half) for a Custom shape. Right-click: presets";
        case VizWaveFolder: return "Drag: gain (x) / bias (y)";
        case VizMbDrive:  return "Drag crossovers. Drag up / down inside a band: its drive";
        case VizSpecDly:  return "Drag bands: time (x) / feedback (y). Scroll: level";
        case VizRoom: case VizPlate: case VizShimmer: case VizAlgo: case VizConv: case VizSpring: return "Drag the onset: pre-delay. Drag the end of the slope: decay (RT60)";
        case VizSimpleDly: case VizPingPong: case VizEcho: case VizPrecDly: case VizWarp: return "Drag the first tap: time (snaps to divisions when synced). Drag the next repeat up / down: feedback";
        case VizWidth:    return "Drag sideways: width";
        case VizMonoBass: return "Drag the handle: crossover";
        case VizHaas:     return "Drag the delayed lane: delay";
        case VizResSupp:  return "Drag handles: frequency range";
        case VizWavetable: case VizWtDrive: return "Drag sideways: table position";
        case VizWah:      return proc.getSlotMode (slot) == 2 ? "Drag the pedal; bottom handles: sweep range" : (proc.getSlotMode (slot) == 1 ? "Drag / double-click points to draw a Custom shape. Right-click: presets" : "Drag the bottom handles: sweep range (Low / High)");
        default: break;
    }
    if (viz::isLfoViz (d->viz)) return "Drag / double-click points to draw a Custom shape. Alt-drag bends. Right-click: presets";
    return {};
}

void VizEditor::paint (juce::Graphics& g)
{
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    viz::drawEffectVisual (g, getLocalBounds().toFloat(), s);
    if (s.d != nullptr)
    {
        // live readout pill
        // reverb / delay label their own handles, so skip the pill there
        const bool labelled = viz::isTimeViz (s.d->viz);
        const auto tag = labelled ? juce::String() : viz::readoutTag (s);
        if (tag.isNotEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            const float tw = juce::jmax (80.0f, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), tag) + 22.0f);
            juce::Rectangle<float> pill ((float) getWidth() - 8.0f - tw, 8.0f, tw, 22.0f);
            // keep it off the draggable handles: try the other corners
            const auto hs = handles (s);
            const auto covers = [&hs] (juce::Rectangle<float> r)
            {
                for (const auto& h : hs) if (r.expanded (10.0f).contains (h.pos)) return true;
                return false;
            };
            for (auto alt : { pill.withY ((float) getHeight() - 30.0f), pill.withX (8.0f), pill.withX (8.0f).withY ((float) getHeight() - 30.0f) })
                if (covers (pill) && ! covers (alt)) pill = alt;
            g.setColour (juce::Colour (0xEE202227));
            g.fillRoundedRectangle (pill, 5.0f);
            g.setColour (juce::Colour (0xFF33363D));
            g.drawRoundedRectangle (pill.reduced (0.5f), 5.0f, 1.0f);
            g.setColour (juce::Colour (0xFFE8E6E1));
            g.drawText (tag, pill.toNearestInt(), juce::Justification::centred, false);
        }
    }
    // drawing into an LFO switches its shape to Custom: say so
    if (s.d != nullptr && viz::isLfoViz (s.d->viz))
    {
        const int si = viz::lfoShapeIndex (*s.d);
        if (si >= 0 && (int) std::lround (phys (si)) == LfoShapes::kCustom)
        {
            juce::Rectangle<float> badge (8.0f, 8.0f, 64.0f, 20.0f);
            g.setColour (s.accent.withAlpha (0.22f));
            g.fillRoundedRectangle (badge, 5.0f);
            g.setColour (s.accent);
            g.drawRoundedRectangle (badge.reduced (0.5f), 5.0f, 1.0f);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText ("CUSTOM", badge.toNearestInt(), juce::Justification::centred, false);
        }
    }
    // EQ Q brackets: a faint span between two upright tabs, Q value above
    {
        const auto hs = handles (s);
        const Handle* qa = nullptr; const Handle* qb = nullptr;
        for (const auto& h : hs) { if (h.id == kQHandle) qa = &h; if (h.id == kQHandle + 1) qb = &h; }
        float fc = 0.0f, q = 0.0f;
        if (qa != nullptr && qb != nullptr && eqBand (s, focusBand, fc, q))
        {
            g.setColour (s.accent.withAlpha (0.12f));
            g.fillRect (juce::Rectangle<float> (qa->pos.x, qa->pos.y - 3.0f, qb->pos.x - qa->pos.x, 6.0f));
            for (const auto* h : { qa, qb })
            {
                g.setColour (s.accent);
                g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 16.0f).withCentre (h->pos), 2.5f);
            }
            g.setColour (juce::Colour (0xFFE8E6E1));
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText ("Q " + juce::String (q, q < 10.0f ? 2 : 1), juce::Rectangle<float> (qb->pos.x + 6.0f, qb->pos.y - 7.0f, 60.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }
    if (hover >= 0)
        for (const auto& h : handles (s))
            if (h.id == hover)
            {
                g.setColour (juce::Colours::white.withAlpha (0.8f));
                g.drawEllipse (h.pos.x - 9.0f, h.pos.y - 9.0f, 18.0f, 18.0f, 1.5f);
            }
}

void VizEditor::mouseMove (const juce::MouseEvent& e)
{
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    const int h = hitHandle (s, e.position);
    if (h != hover) { hover = h; repaint(); }
    // hovering an EQ band brings up its Q brackets
    if ((s.d != nullptr && (s.d->viz == VizEq3 || s.d->viz == VizEq8)) && h >= 0 && h < kQHandle && h != focusBand) { focusBand = h; repaint(); }
    setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

// ------------------------------------------------------------------ mouse
void VizEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto* d = def();
    if (d == nullptr) return;
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    downPos = e.position;
    drag = Drag::None;
    const int h = hitHandle (s, e.position);
    if (e.mods.isPopupMenu())
    {
        if (h >= 0) handleMenu (h); else backgroundMenu();
        return;
    }
    if (h < 0 && d->viz == VizHarmRes)
    {
        // keyboard: toggle a pitch (the set becomes Custom), drag for its level
        const int pc = viz::harmResKeyAt (viz::harmResKeys (plot(), true), e.position);
        if (pc >= 0)
        {
            ensureCustomSet();
            setData (pc, data (pc) > 0.5f ? 0.0f : 1.0f);
            drag = Drag::Keys; dragPoint = pc;
            edited();
            return;
        }
    }
    if (h >= 0)
    {
        drag = Drag::Handle; dragId = h;
        // remember the start values the drag works from
        const auto r = plot();
        switch (d->viz)
        {
            case VizEq3:
                if (h >= kQHandle) beginGesture ({ focusBand == 0 ? 11 : (focusBand == 1 ? 6 : 12) });
                else { beginGesture ({ 3 + h, h }); focusBand = h; }
                break;
            case VizEq8: if (h < kQHandle) focusBand = h; break;
            case VizMultimode: case VizSvf: case VizLadder: case VizVintage: beginGesture ({ 0, 1 }); startVals[1] = phys (1); break;
            case VizComb: beginGesture ({ 0, 1 }); startVals[1] = phys (1); break;
            case VizDeess: beginGesture ({ 0 }); break;
            case VizAllpass: beginGesture ({ 0, 1 }); startVals[1] = phys (1); break;
            case VizComp: case VizSidechain: case VizExpander: beginGesture ({ 0, 1 }); startVals[1] = phys (1); break;
            case VizGate: beginGesture ({ 0 }); break;
            case VizLimit: beginGesture ({ 1, 5 }); startVals[0] = phys (1); startVals[1] = phys (5); break;
            case VizTransient:
                frozenSpan = viz::transientSpanMs (s);
                beginGesture ({ h == 0 ? 0 : (h == 1 ? 6 : (h == 2 ? 1 : 5)) });
                break;
            case VizHarmRes: beginGesture ({ 3 + h }); break;
            case VizMbGate: if (h >= 10) beginGesture ({ h - 10 }); break;
            case VizMbDrive: beginGesture ({ 3 + h }); break;
            case VizMonoBass: case VizHaas: beginGesture ({ 0 }); startVals[0] = phys (0); break;
            case VizWah: beginGesture ({ h == 0 ? 0 : 2 + h }); startVals[0] = phys (0); break;
            case VizResSupp: beginGesture ({ 2 + h }); break;
            case VizSpecDly: startVals[0] = data (h * 4); startVals[1] = data (h * 4 + 1); break;
            case VizRoom: case VizPlate: case VizShimmer: case VizAlgo: case VizConv: case VizSpring:
            {
                int iDecay = 0, iPre = -1;
                viz::decayShownRt (s, iDecay, iPre);
                frozenSpan = viz::decaySpanMs (s);
                startVals[0] = (float) iDecay; startVals[1] = (float) iPre;
                beginGesture ({ h == 1 ? iDecay : iPre });
                break;
            }
            case VizSimpleDly: case VizPingPong: case VizEcho: case VizPrecDly: case VizWarp:
            {
                frozenSpan = viz::delaySpanMs (s);
                startVals[0] = viz::delayBpm (s);
                const int iFb = d->viz == VizWarp ? 2 : (d->viz == VizPrecDly && s.mode == 0 ? 9 : 3);
                startVals[2] = (float) iFb;
                if (h == 1) beginGesture ({ iFb });
                else beginGesture ({ d->viz != VizWarp && (int) std::lround (phys (0)) == 1 ? 2 : 1 });
                break;
            }
            default: break;
        }
        return;
    }
    // no handle: curve / steps / bars / free drags
    switch (d->viz)
    {
        case VizDuck: case VizWaveShaper:
        case VizAutoPan: case VizTremolo: case VizChorus: case VizFlanger: case VizPhaser: case VizVibrato: case VizWobble:
            if (d->viz == VizWaveShaper && e.position.x < curveArea().getX()) return;
            if (viz::isLfoViz (d->viz) || d->viz == VizWaveShaper) ensureCustomCurve();
            drag = Drag::Curve;
            dragPoint = hitCurvePoint (e.position);
            if (dragPoint < 0) beginBend (e.position);
            break;
        case VizWah:
            if (s.mode == 1)
            {
                ensureCustomCurve();
                drag = Drag::Curve;
                dragPoint = hitCurvePoint (e.position);
                if (dragPoint < 0) beginBend (e.position);
            }
            break;
        case VizTrance: drag = Drag::Steps; setStepOrBar (e.position); break;
        case VizAdditive: case VizSpecComp: drag = Drag::Bars; setStepOrBar (e.position); break;
        case VizWaveFolder: drag = Drag::Free; startVals[0] = phys (0); startVals[1] = phys (1); beginGesture ({ 0, 1 }); break;
        case VizWidth: drag = Drag::Free; startVals[0] = phys (0); beginGesture ({ 0 }); break;
        case VizWavetable: case VizWtDrive: drag = Drag::Free; startVals[0] = phys (0); beginGesture ({ 0 }); break;
        case VizMbDrive:
        {
            // vertical drag inside a band sets that band's drive
            const auto r = plot();
            const float x1 = viz::xOfHz (r, phys (3)), x2 = viz::xOfHz (r, juce::jmax (phys (4), phys (3) * 1.5f));
            dragId = e.position.x < x1 ? 0 : (e.position.x < x2 ? 1 : 2);
            startVals[0] = phys (dragId);
            drag = Drag::Free;
            beginGesture ({ dragId });
            break;
        }
        default: break;
    }
}

void VizEditor::setStepOrBar (juce::Point<float> p)
{
    const auto* d = def();
    const auto r = plot();
    if (d->viz == VizTrance)
    {
        const int steps = juce::jlimit (1, Shape::kSteps, (int) std::lround (phys (1)));
        const int k = juce::jlimit (0, steps - 1, (int) ((p.x - r.getX()) / r.getWidth() * (float) steps));
        const float depth = juce::jmax (0.01f, phys (2));
        const float lvl = clamp01 ((r.getBottom() - 4.0f - p.y) / (r.getHeight() - 8.0f));
        setData (k, clamp01 (1.0f - (1.0f - lvl) / depth));
    }
    else if (d->viz == VizAdditive)
    {
        const int k = juce::jlimit (0, Shape::kHarmonics - 1, (int) ((p.x - r.getX()) / r.getWidth() * (float) Shape::kHarmonics));
        setData (k, clamp01 ((r.getBottom() - 16.0f - p.y) / (r.getHeight() - 18.0f)));
    }
    else if (d->viz == VizSpecComp)
    {
        const int sel = (int) std::lround (phys (4));
        const int nb = sel == 0 ? 4 : (sel == 1 ? 8 : 16);
        const int k = juce::jlimit (0, nb - 1, (int) ((p.x - r.getX()) / r.getWidth() * (float) nb));
        setData (k, juce::jlimit (-24.0f, 24.0f, (r.getCentreY() - p.y) / (r.getHeight() * 0.5f - 2.0f) * 24.0f));
    }
    edited();
}

void VizEditor::mouseDrag (const juce::MouseEvent& e)
{
    const auto* d = def();
    if (d == nullptr || e.mods.isPopupMenu()) return;
    if (drag == Drag::Handle) { dragHandle (dragId, e); edited(); return; }
    if (drag == Drag::Steps || drag == Drag::Bars) { setStepOrBar (e.position); return; }
    if (drag == Drag::Keys)
    {
        const auto kr = viz::harmResKeyRect (viz::harmResKeys (plot(), true), dragPoint);
        setData (dragPoint, juce::jlimit (0.0f, 1.0f, (kr.getBottom() - e.position.y) / kr.getHeight()));
        edited();
        return;
    }
    const float dx = e.position.x - downPos.x, dy = e.position.y - downPos.y;
    if (drag == Drag::Free)
    {
        switch (d->viz)
        {
            case VizWaveFolder:
                setNorm (0, mapParam (d->p[0], juce::jlimit (1.0f, 20.0f, startVals[0] * std::pow (2.0f, dx / 120.0f))));
                setPhys (1, juce::jlimit (-1.0f, 1.0f, startVals[1] - dy / 150.0f));
                break;
            case VizWidth:      setPhys (0, juce::jlimit (0.0f, 2.0f, startVals[0] + dx / 200.0f)); break;
            case VizWavetable: case VizWtDrive: setPhys (0, clamp01 (startVals[0] + dx / plot().getWidth())); break;
            case VizMbDrive:    setPhys (dragId, juce::jlimit (0.0f, 24.0f, startVals[0] - dy / 8.0f)); break;
            default: break;
        }
        edited();
        return;
    }
    if (drag == Drag::Curve)
    {
        float buf[kDataSize];
        proc.getSlotDataAll (slot, buf);
        const auto r = curveArea();
        const int n = CurvePoints::count (buf);
        if (dragPoint >= 0)
        {
            const bool endPt = dragPoint == 0 || dragPoint == n - 1;
            if (e.mods.isAltDown() && dragPoint < n - 1)
            {
                // alt-drag bends the segment that starts at this point
                buf[3 + dragPoint * 3] = juce::jlimit (-1.0f, 1.0f, -dy / 100.0f);
            }
            else
            {
                float x = clamp01 ((e.position.x - r.getX()) / r.getWidth());
                if (endPt) x = dragPoint == 0 ? 0.0f : 1.0f;
                else x = juce::jlimit (buf[1 + (dragPoint - 1) * 3] + 0.005f, buf[1 + (dragPoint + 1) * 3] - 0.005f, x);
                buf[1 + dragPoint * 3] = x;
                buf[2 + dragPoint * 3] = curveYToValue (e.position.y);
                // LFOs loop: keep the end level equal to the start level
                if (viz::isLfoViz (d->viz) && endPt) { buf[2] = buf[2 + dragPoint * 3]; buf[2 + (n - 1) * 3] = buf[2 + dragPoint * 3]; }
            }
            proc.setSlotDataBlock (slot, 0, buf, CurvePoints::kSize);
        }
        else if (dragId >= 0 && dragId < n - 1)
        {
            // dragging between points bends that segment towards the mouse
            buf[3 + dragId * 3] = juce::jlimit (-1.0f, 1.0f, startVals[0] + dy / 100.0f * startVals[1]);
            proc.setSlotDataBlock (slot, 0, buf, CurvePoints::kSize);
        }
        edited();
    }
}

void VizEditor::beginBend (juce::Point<float> p)
{
    float buf[kDataSize];
    proc.getSlotDataAll (slot, buf);
    const auto r = curveArea();
    const int n = CurvePoints::count (buf);
    const float x = clamp01 ((p.x - r.getX()) / r.getWidth());
    dragId = -1;
    for (int i = 0; i < n - 1; ++i)
        if (x >= buf[1 + i * 3] && x <= buf[1 + (i + 1) * 3]) { dragId = i; break; }
    if (dragId < 0) return;
    // which way a positive bend moves the segment's middle on screen
    const float y0 = buf[2 + dragId * 3], y1 = buf[2 + (dragId + 1) * 3];
    const float moved = curveValueToY (y0 + (y1 - y0) * CurvePoints::bend (0.5f, 0.3f)) - curveValueToY (0.5f * (y0 + y1));
    startVals[0] = buf[3 + dragId * 3];
    startVals[1] = std::abs (moved) < 0.01f ? 0.0f : (moved > 0.0f ? 1.0f : -1.0f);
}

void VizEditor::dragHandle (int id, const juce::MouseEvent& e)
{
    const auto* d = def();
    const auto r = plot();
    float lo, hi;
    viz::responseRange (d->viz, lo, hi);
    const float dx = e.position.x - downPos.x, dy = e.position.y - downPos.y;
    const float hz = viz::hzOfX (r, e.position.x);
    if ((d->viz == VizEq3 || d->viz == VizEq8) && id >= kQHandle)
    {
        // Q bracket: the distance from the band centre is half the bandwidth
        std::shared_ptr<const Wavetable> holder;
        const auto s = state (holder);
        float fc = 0.0f, q0 = 0.0f;
        if (! eqBand (s, focusBand, fc, q0)) return;
        const float q = halfOctToQ (std::abs (std::log2 (hz / fc)));
        if (d->viz == VizEq3)
        {
            const int qi = focusBand == 0 ? 11 : (focusBand == 1 ? 6 : 12);
            setPhys (qi, juce::jlimit (d->p[qi].lo, d->p[qi].hi, q));
        }
        else setData (focusBand * Shape::kEqStride + 4, juce::jlimit (0.1f, 18.0f, q));
        return;
    }
    switch (d->viz)
    {
        case VizEq3:
        {
            setPhys (3 + id, hz);
            const int type = (int) std::lround (phys (8 + id));
            const bool noGain = (id == 0 && type == 0) || (id == 2 && type == 1) || (id == 1 && type != 0);
            if (! noGain) setPhys (id, juce::jlimit (-18.0f, 18.0f, viz::dbOfY (r, e.position.y, lo, hi)));
            break;
        }
        case VizEq8:
        {
            float b[Shape::kEqStride];
            for (int i = 0; i < Shape::kEqStride; ++i) b[i] = data (id * Shape::kEqStride + i);
            b[2] = juce::jlimit (20.0f, 20000.0f, hz);
            const float scale = juce::jmax (0.05f, phys (1));
            if ((int) std::lround (b[1]) <= 2) b[3] = juce::jlimit (-24.0f, 24.0f, viz::dbOfY (r, e.position.y, lo, hi) / scale);
            b[0] = 1.0f;
            proc.setSlotDataBlock (slot, id * Shape::kEqStride, b, Shape::kEqStride);
            break;
        }
        case VizMultimode: case VizSvf: case VizLadder: case VizVintage:
            setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, hz));
            setPhys (1, clamp01 (startVals[1] - dy / 220.0f));
            break;
        case VizComb:
            setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, hz));
            setPhys (1, juce::jlimit (-0.98f, 0.98f, startVals[1] - dy / 200.0f));
            break;
        case VizDeess: setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, hz)); break;
        case VizAllpass:
            setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, hz));
            setPhys (1, juce::jlimit (0.3f, 8.0f, startVals[1] * std::pow (2.0f, -dy / 80.0f)));
            break;
        case VizComp: case VizSidechain: case VizExpander:
            setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, viz::kDynLo + (e.position.x - r.getX()) / r.getWidth() * (viz::kDynHi - viz::kDynLo)));
            setPhys (1, juce::jlimit (d->p[1].lo, d->p[1].hi, startVals[1] * std::pow (2.0f, dy / 60.0f)));
            break;
        case VizGate:
            setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, viz::kDynLo + (e.position.x - r.getX()) / r.getWidth() * (viz::kDynHi - viz::kDynLo)));
            break;
        case VizLimit:
            setPhys (1, juce::jlimit (d->p[1].lo, d->p[1].hi, startVals[0] + dx / r.getWidth() * (viz::kDynHi - viz::kDynLo)));
            setPhys (5, juce::jlimit (d->p[5].lo, d->p[5].hi, startVals[1] - dy / r.getHeight() * (viz::kDynHi - viz::kDynLo)));
            break;
        case VizOtt:
        {
            const int b = id / 2, k = id % 2;
            const float cw = r.getWidth() / 3.0f;
            auto col = juce::Rectangle<float> (r.getX() + cw * (float) b, r.getY(), cw, r.getHeight()).reduced (14.0f, 2.0f);
            col.removeFromBottom (14.0f);
            float v = juce::jlimit (-80.0f, 0.0f, viz::dbOfY (col, e.position.y, -80.0f, 0.0f));
            // keep the up threshold below the down threshold
            if (k == 0) v = juce::jmax (v, data (b * 4 + 1) + 1.0f); else v = juce::jmin (v, data (b * 4) - 1.0f);
            setData (b * 4 + k, v);
            break;
        }
        case VizTransient:
        {
            auto tr = r.withTrimmedRight (10.0f);
            const float ms = viz::msOfX (tr, e.position.x, frozenSpan);
            const float db = viz::dbOfY (tr, e.position.y, -viz::kTsRangeDb, viz::kTsRangeDb);
            if (id == 0)      setPhys (0, juce::jlimit (-1.0f, 1.0f, db / 12.0f));
            else if (id == 1) setPhys (6, juce::jlimit (d->p[6].lo, d->p[6].hi, ms));
            else if (id == 2) setPhys (1, juce::jlimit (-1.0f, 1.0f, db / 12.0f));
            else              setPhys (5, juce::jlimit (d->p[5].lo, d->p[5].hi, ms - phys (6)));
            break;
        }
        case VizHarmRes:
            setPhys (3 + id, juce::jlimit (d->p[3 + id].lo, d->p[3 + id].hi, viz::hzOfX (viz::harmResCurve (r, true), e.position.x)));
            break;
        case VizMbGate:
            if (id >= 10)
            {
                // a crossover stays between its neighbours
                const int k = id - 10;
                std::shared_ptr<const Wavetable> holder;
                const auto s = state (holder);
                const float lo = k > 0 ? Shape::mbgCrossover (s.p, k - 1) * 1.15f : d->p[k].lo;
                const float hi = k < Shape::kMbgBands - 2 ? Shape::mbgCrossover (s.p, k + 1) / 1.15f : d->p[k].hi;
                setPhys (k, juce::jlimit (juce::jmax (lo, d->p[k].lo), juce::jmin (hi, d->p[k].hi), hz));
            }
            else
                setData (id * Shape::kMbgStride, juce::jlimit (Shape::kMbgFloorDb, 0.0f, viz::dbOfY (r, e.position.y, viz::kMbgLo, viz::kMbgHi) - phys (4)));
            break;
        case VizMbDrive:
            if (id == 0) setPhys (3, juce::jlimit (d->p[3].lo, d->p[3].hi, hz));
            else setPhys (4, juce::jlimit (d->p[4].lo, d->p[4].hi, hz));
            break;
        case VizMonoBass: setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, viz::hzOfX (r.withTrimmedBottom (18.0f), e.position.x))); break;
        case VizHaas:     setPhys (0, juce::jlimit (d->p[0].lo, d->p[0].hi, startVals[0] + dx / (r.getWidth() - 34.0f) * 45.0f)); break;
        case VizWah:
        {
            if (id > 0) { setPhys (2 + id, juce::jlimit (d->p[2 + id].lo, d->p[2 + id].hi, hz)); break; }
            const float lo2 = phys (3), hi2 = phys (4);
            setPhys (0, clamp01 (std::log (juce::jlimit (lo2, hi2, hz) / lo2) / std::log (hi2 / lo2)));
            break;
        }
        case VizResSupp:
        {
            const float flo = 60.0f, fhi = 14000.0f;
            const float f = flo * std::pow (fhi / flo, clamp01 ((e.position.x - r.getX()) / r.getWidth()));
            setPhys (2 + id, juce::jlimit (d->p[2 + id].lo, d->p[2 + id].hi, f));
            break;
        }
        case VizSpecDly:
            setData (id * 4, juce::jlimit (0.05f, 8.0f, startVals[0] * std::pow (2.0f, dx / 100.0f)));
            setData (id * 4 + 1, juce::jlimit (0.0f, 2.0f, startVals[1] - dy / 100.0f));
            break;
        case VizRoom: case VizPlate: case VizShimmer: case VizAlgo: case VizConv: case VizSpring:
        {
            // handles sit where the mouse is: pre-delay at the onset, RT60 where the slope meets the floor
            const int iDecay = (int) startVals[0], iPre = (int) startVals[1];
            const float ms = viz::msOfX (r, e.position.x, frozenSpan);
            if (id == 1) setPhys (iDecay, juce::jlimit (d->p[iDecay].lo, d->p[iDecay].hi, (ms - (iPre >= 0 ? phys (iPre) : 0.0f)) * 0.001f));
            else if (iPre >= 0) setPhys (iPre, juce::jlimit (d->p[iPre].lo, d->p[iPre].hi, ms));
            break;
        }
        case VizSimpleDly: case VizPingPong: case VizEcho: case VizPrecDly: case VizWarp:
        {
            if (id == 1)
            {
                // the next repeat's height is the feedback
                const int iFb = (int) startVals[2];
                const float gain = juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / (r.getHeight() * 0.85f));
                if (iFb == 9) setPhys (9, juce::jlimit (-24.0f, 0.0f, juce::Decibels::gainToDecibels (gain, -24.0f)));
                else setPhys (iFb, juce::jlimit (d->p[iFb].lo, d->p[iFb].hi, gain));
                break;
            }
            const float ms = viz::msOfX (r, e.position.x, frozenSpan);
            if (d->viz == VizWarp || (int) std::lround (phys (0)) == 0)
                setPhys (1, juce::jlimit (d->p[1].lo, d->p[1].hi, ms));
            else
            {
                // synced: snap to the nearest division at the current tempo
                int best = 0; float bestErr = 1e9f;
                for (int i = 0; i < 15; ++i)
                {
                    const float err = std::abs (std::log (juce::jmax (1.0f, ms) / (viz::divisionBeats (i) * 60000.0f / startVals[0])));
                    if (err < bestErr) { bestErr = err; best = i; }
                }
                setPhys (2, (float) best);
            }
            break;
        }
        default: break;
    }
}

void VizEditor::mouseUp (const juce::MouseEvent&)
{
    endGestures();
    frozenSpan = 0.0f;
    drag = Drag::None;
    dragPoint = -1;
    dragId = -1;
}

void VizEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto* d = def();
    if (d == nullptr) return;
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    const auto r = plot();
    if (d->viz == VizMbGate)
    {
        // double-click a band: on / off
        for (int b = 0; b < Shape::kMbgBands; ++b)
            if (viz::mbgBandX (r, s, b).contains (e.position.x)) { toggleMbgBand (b); return; }
    }
    if (d->viz == VizEq8)
    {
        const int h = hitHandle (s, e.position);
        if (h >= 0) { setData (h * Shape::kEqStride, data (h * Shape::kEqStride) > 0.5f ? 0.0f : 1.0f); edited(); return; }
        // enable the first disabled band at the click
        for (int b = 0; b < Shape::kEqBands; ++b)
            if (data (b * Shape::kEqStride) < 0.5f)
            {
                float lo, hi; viz::responseRange (VizEq8, lo, hi);
                const float v[5] = { 1.0f, 0.0f, viz::hzOfX (r, e.position.x), juce::jlimit (-24.0f, 24.0f, viz::dbOfY (r, e.position.y, lo, hi) / juce::jmax (0.05f, phys (1))), 1.0f };
                proc.setSlotDataBlock (slot, b * Shape::kEqStride, v, 5);
                edited();
                return;
            }
        return;
    }
    const bool curve = d->viz == VizDuck || d->viz == VizWaveShaper || viz::isLfoViz (d->viz);
    if (! curve) return;
    if (viz::isLfoViz (d->viz) || d->viz == VizWaveShaper) ensureCustomCurve();
    float buf[kDataSize];
    proc.getSlotDataAll (slot, buf);
    const int n = CurvePoints::count (buf);
    const int hit = hitCurvePoint (e.position);
    if (hit > 0 && hit < n - 1)
    {
        // remove an inner point
        for (int i = hit; i < n - 1; ++i) for (int k = 0; k < 3; ++k) buf[1 + i * 3 + k] = buf[1 + (i + 1) * 3 + k];
        buf[0] = (float) (n - 1);
    }
    else if (hit < 0 && n < CurvePoints::kMaxPts)
    {
        // add a point at the click
        const auto ca = curveArea();
        const float x = clamp01 ((e.position.x - ca.getX()) / ca.getWidth());
        int at = 1;
        while (at < n && buf[1 + at * 3] < x) ++at;
        for (int i = n; i > at; --i) for (int k = 0; k < 3; ++k) buf[1 + i * 3 + k] = buf[1 + (i - 1) * 3 + k];
        buf[1 + at * 3] = x; buf[2 + at * 3] = curveYToValue (e.position.y); buf[3 + at * 3] = 0.0f;
        buf[0] = (float) (n + 1);
    }
    proc.setSlotDataBlock (slot, 0, buf, CurvePoints::kSize);
    edited();
}

void VizEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto* d = def();
    if (d == nullptr) return;
    std::shared_ptr<const Wavetable> holder;
    const auto s = state (holder);
    int h = hitHandle (s, e.position);
    if (h >= kQHandle) h = focusBand;   // scrolling a Q bracket scrolls its band
    if (h < 0) return;
    wheelHandle (h, w.deltaY);
    edited();
}

void VizEditor::toggleMbgBand (int b)
{
    setData (b * Shape::kMbgStride + 2, data (b * Shape::kMbgStride + 2) > 0.5f ? 0.0f : 1.0f);
    edited();
}

void VizEditor::wheelHandle (int id, float delta)
{
    const auto* d = def();
    const float f = std::pow (1.25f, delta * 4.0f);
    switch (d->viz)
    {
        case VizMbGate:   // scroll a band: its range (how far a closed band drops)
            if (id < Shape::kMbgBands)
                setData (id * Shape::kMbgStride + 1, juce::jlimit (Shape::kMbgFloorDb, 0.0f, data (id * Shape::kMbgStride + 1) + delta * 12.0f));
            break;
        case VizEq3:
        {
            const int qi = id == 0 ? 11 : (id == 1 ? 6 : 12);
            setPhys (qi, juce::jlimit (d->p[qi].lo, d->p[qi].hi, phys (qi) * f));
            break;
        }
        case VizEq8: setData (id * Shape::kEqStride + 4, juce::jlimit (0.1f, 18.0f, data (id * Shape::kEqStride + 4) * f)); break;
        case VizMultimode: case VizSvf: case VizLadder: case VizVintage: setPhys (1, clamp01 (phys (1) + delta * 0.25f)); break;
        case VizComp: case VizSidechain: case VizExpander: setPhys (1, juce::jlimit (d->p[1].lo, d->p[1].hi, phys (1) * f)); break;
        case VizOtt:
        {
            const int idx = (id / 2) * 4 + 2 + (id % 2);
            setData (idx, juce::jlimit (1.0f, 100.0f, data (idx) * f));
            break;
        }
        case VizSpecDly: setData (id * 4 + 2, juce::jlimit (-60.0f, 12.0f, data (id * 4 + 2) + delta * 12.0f)); break;
        default: break;
    }
}

void VizEditor::handleMenu (int id)
{
    const auto* d = def();
    if (id >= kQHandle) id = focusBand;
    if (id < 0) return;
    juce::PopupMenu m;
    if (d->viz == VizMbGate)
    {
        if (id < Shape::kMbgBands) toggleMbgBand (id);
        return;
    }
    if (d->viz == VizEq3)
    {
        const int ci2 = 8 + id;
        const auto& sp = d->p[ci2];
        for (int c = 0; c < sp.numChoices; ++c) m.addItem (c + 1, sp.choices[c], true, (int) std::lround (phys (ci2)) == c);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, ci2] (int r)
        {
            if (r > 0) { if (const auto* dd = def()) setNorm (ci2, mapParam (dd->p[ci2], (float) (r - 1))); edited(); }
        });
        return;
    }
    if (d->viz == VizEq8)
    {
        const int base = id * Shape::kEqStride;
        for (int t = 0; t < Shape::kNumEqTypes; ++t) m.addItem (t + 1, Shape::kEqTypes[t], true, (int) std::lround (data (base + 1)) == t);
        m.addSeparator();
        m.addItem (100, data (base) > 0.5f ? "Disable band" : "Enable band");
        m.addItem (101, "Reset gain / Q");
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, base] (int r)
        {
            if (r >= 1 && r <= Shape::kNumEqTypes) { setData (base + 1, (float) (r - 1)); setData (base, 1.0f); }
            else if (r == 100) setData (base, data (base) > 0.5f ? 0.0f : 1.0f);
            else if (r == 101) { setData (base + 3, 0.0f); setData (base + 4, 1.0f); }
            edited();
        });
        return;
    }
    backgroundMenu();
}

void VizEditor::backgroundMenu()
{
    const auto* d = def();
    if (d == nullptr) return;
    juce::PopupMenu m;
    if (d->viz == VizMbGate)
    {
        // right-click a band (an off band has no handle): on / off
        std::shared_ptr<const Wavetable> holder;
        const auto s = state (holder);
        const auto pos = getMouseXYRelative().toFloat();
        for (int b = 0; b < Shape::kMbgBands; ++b)
            if (viz::mbgBandX (plot(), s, b).contains (pos.x)) { toggleMbgBand (b); return; }
        return;
    }
    if (d->viz == VizTrance)
    {
        m.addSectionHeader ("Step pattern");
        const char* names[] = { "Classic", "Offbeat", "Gallop", "Eighths", "Sparse", "All on", "Random", "Clear" };
        for (int i = 0; i < 8; ++i) m.addItem (i + 1, names[i]);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this] (int r)
        {
            if (r <= 0) return;
            static constexpr int pats[5][16] = { { 1,0,1,1, 0,1,1,0, 1,1,0,1, 1,0,1,1 }, { 0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1 },
                                                 { 1,0,1,1, 1,0,1,1, 1,0,1,1, 1,0,1,1 }, { 1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0 },
                                                 { 1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,1 } };
            float v[Shape::kSteps];
            juce::Random rnd;
            for (int k = 0; k < Shape::kSteps; ++k)
                v[k] = r <= 5 ? (float) pats[r - 1][k % 16] : (r == 6 ? 1.0f : (r == 7 ? (rnd.nextFloat() < 0.6f ? 1.0f : 0.0f) : 0.0f));
            proc.setSlotDataBlock (slot, 0, v, Shape::kSteps);
            edited();
        });
        return;
    }
    if (d->viz == VizAdditive)
    {
        m.addSectionHeader ("Harmonics");
        const char* names[] = { "Warm (2nd + 3rd)", "Even only", "Odd only", "Saw-like (1/n)", "Square-like (odd 1/n)", "Clear" };
        for (int i = 0; i < 6; ++i) m.addItem (i + 1, names[i]);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this] (int r)
        {
            if (r <= 0) return;
            float v[Shape::kHarmonics] {};
            for (int k = 0; k < Shape::kHarmonics; ++k)
            {
                const int h = k + 2;
                switch (r)
                {
                    case 1: v[k] = h == 2 ? 0.6f : (h == 3 ? 0.3f : 0.0f); break;
                    case 2: v[k] = (h % 2 == 0) ? 1.0f / (float) (h / 2) : 0.0f; break;
                    case 3: v[k] = (h % 2 == 1) ? 2.0f / (float) h : 0.0f; break;
                    case 4: v[k] = 1.5f / (float) h; break;
                    case 5: v[k] = (h % 2 == 1) ? 1.5f / (float) h : 0.0f; break;
                    default: v[k] = 0.0f; break;
                }
            }
            proc.setSlotDataBlock (slot, 0, v, Shape::kHarmonics);
            edited();
        });
        return;
    }
    if (d->viz == VizSpecComp)
    {
        m.addSectionHeader ("Threshold offsets");
        m.addItem (1, "Flat"); m.addItem (2, "Tilt: highs compress more"); m.addItem (3, "Tilt: lows compress more"); m.addItem (4, "Smile (mids compress more)");
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this] (int r)
        {
            if (r <= 0) return;
            float v[16];
            for (int b = 0; b < 16; ++b)
            {
                const float t = (float) b / 15.0f * 2.0f - 1.0f;
                v[b] = r == 1 ? 0.0f : (r == 2 ? -t * 12.0f : (r == 3 ? t * 12.0f : (std::abs (t) - 0.5f) * 16.0f));
            }
            proc.setSlotDataBlock (slot, 0, v, 16);
            edited();
        });
        return;
    }
    const bool curve = d->viz == VizDuck || d->viz == VizWaveShaper || viz::isLfoViz (d->viz);
    if (curve)
    {
        m.addSectionHeader ("Curve");
        if (d->viz == VizDuck)
        {
            m.addItem (1, "Classic pump"); m.addItem (2, "Smooth"); m.addItem (3, "Hard cut"); m.addItem (4, "Double");
        }
        else if (d->viz == VizWaveShaper)
        {
            m.addItem (1, "Soft"); m.addItem (2, "Hard knee"); m.addItem (3, "Fold"); m.addItem (4, "Steps");
        }
        else
        {
            m.addItem (1, "Sine"); m.addItem (2, "Triangle"); m.addItem (3, "Saw"); m.addItem (4, "Square"); m.addItem (5, "Swing");
        }
        m.addSeparator();
        m.addItem (99, "Tip: double-click adds / removes points, Alt-drag bends", false);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this] (int r)
        {
            const auto* dd = def();
            if (r <= 0 || r == 99 || dd == nullptr) return;
            float buf[CurvePoints::kSize] {};
            if (dd->viz == VizDuck)
            {
                switch (r)
                {
                    case 1: CurvePoints::set (buf, { { 0.0f, 1.0f, 0.0f }, { 0.04f, 1.0f, 0.55f }, { 0.6f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }); break;
                    case 2: CurvePoints::set (buf, { { 0.0f, 1.0f, 0.3f }, { 0.9f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }); break;
                    case 3: CurvePoints::set (buf, { { 0.0f, 1.0f, 0.0f }, { 0.3f, 1.0f, 0.0f }, { 0.32f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }); break;
                    default: CurvePoints::set (buf, { { 0.0f, 1.0f, 0.5f }, { 0.4f, 0.0f, 0.0f }, { 0.5f, 1.0f, 0.5f }, { 0.9f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }); break;
                }
            }
            else if (dd->viz == VizWaveShaper)
            {
                ensureCustomCurve();
                switch (r)
                {
                    case 1: CurvePoints::set (buf, { { 0.0f, 0.0f, 0.0f }, { 0.35f, 0.6f, -0.3f }, { 0.7f, 0.85f, 0.0f }, { 1.0f, 0.95f, 0.0f } }); break;
                    case 2: CurvePoints::set (buf, { { 0.0f, 0.0f, 0.0f }, { 0.6f, 0.9f, 0.0f }, { 1.0f, 1.0f, 0.0f } }); break;
                    case 3: CurvePoints::set (buf, { { 0.0f, 0.0f, 0.0f }, { 0.4f, 0.9f, 0.0f }, { 0.7f, -0.3f, 0.0f }, { 1.0f, 0.6f, 0.0f } }); break;
                    default: CurvePoints::set (buf, { { 0.0f, 0.0f, 0.0f }, { 0.25f, 0.0f, 0.0f }, { 0.26f, 0.33f, 0.0f }, { 0.5f, 0.33f, 0.0f }, { 0.51f, 0.66f, 0.0f }, { 0.75f, 0.66f, 0.0f }, { 0.76f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } }); break;
                }
            }
            else
            {
                ensureCustomCurve();
                switch (r)
                {
                    case 1: LfoShapes::defaultCustom (buf); break;
                    case 2: CurvePoints::set (buf, { { 0.0f, 0.0f, 0.0f }, { 0.25f, 1.0f, 0.0f }, { 0.75f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }); break;
                    case 3: CurvePoints::set (buf, { { 0.0f, -1.0f, 0.0f }, { 0.99f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f } }); break;
                    case 4: CurvePoints::set (buf, { { 0.0f, 1.0f, 0.0f }, { 0.49f, 1.0f, 0.0f }, { 0.5f, -1.0f, 0.0f }, { 0.99f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } }); break;
                    default: CurvePoints::set (buf, { { 0.0f, -1.0f, 0.4f }, { 0.66f, 1.0f, -0.4f }, { 1.0f, -1.0f, 0.0f } }); break;
                }
            }
            proc.setSlotDataBlock (slot, 0, buf, CurvePoints::kSize);
            edited();
        });
    }
}
