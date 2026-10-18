#include "Visuals.h"
#include "dsp/HarmRes.h"
#include <cmath>

namespace viz
{
using namespace efx;

namespace
{
    const juce::Colour kGrid (0xFF262930), kAxis (0xFF3A3D45), kDim (0xFF6C6A66), kText (0xFFB9B5AF), kRed (0xFFE88A8A), kAmber (0xFFE8B86A);

    juce::Font font (float h, bool bold = true) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }
    float stroke (const State& s) { return s.detailed ? 2.2f : 1.6f; }
    bool delaySynced (const State& s) { return s.d->viz != VizWarp && s.I (0) == 1; }
    void timeGrid (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float span);   // below

    void grid (juce::Graphics& g, juce::Rectangle<float> r, int cols, int rows)
    {
        g.setColour (kGrid);
        for (int i = 1; i < cols; ++i) g.fillRect (r.getX() + r.getWidth() * (float) i / (float) cols, r.getY(), 1.0f, r.getHeight());
        for (int i = 1; i < rows; ++i) g.fillRect (r.getX(), r.getY() + r.getHeight() * (float) i / (float) rows, r.getWidth(), 1.0f);
    }

    void freqGrid (juce::Graphics& g, juce::Rectangle<float> r, bool labels)
    {
        g.setColour (kGrid);
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            g.fillRect (xOfHz (r, f), r.getY(), 1.0f, r.getHeight());
        if (labels)
        {
            g.setColour (kDim);
            g.setFont (font (9.0f, false));
            for (auto [f, t] : { std::pair<float, const char*> { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } })
                g.drawText (t, juce::Rectangle<float> (xOfHz (r, f) + 3.0f, r.getBottom() - 13.0f, 30.0f, 12.0f), juce::Justification::left);
        }
    }

    void dbGrid (juce::Graphics& g, juce::Rectangle<float> r, float lo, float hi, bool labels)
    {
        const float step = (hi - lo) > 40.0f ? 12.0f : 6.0f;
        g.setFont (font (9.0f, false));
        for (float db = std::ceil (lo / step) * step; db <= hi; db += step)
        {
            if (db <= lo + 1.0f) continue;
            const float y = yOfDb (r, db, lo, hi);
            g.setColour (std::abs (db) < 0.01f ? kAxis : kGrid);
            g.fillRect (r.getX(), y, r.getWidth(), 1.0f);
            if (labels && std::abs (db) > 0.01f)
            {
                g.setColour (kDim);
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (r.getX() + 4.0f, y - 12.0f, 40.0f, 11.0f), juce::Justification::left);
            }
        }
    }

    void label (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& s, juce::Justification j, float h = 11.0f, juce::Colour c = kText)
    {
        g.setColour (c);
        g.setFont (font (h));
        g.drawFittedText (s, r.reduced (6.0f, 4.0f).toNearestInt(), j, 1, 0.6f);   // long text (chord names, 6-note spellings) squeezes to fit
    }

    template <typename Fn>
    juce::Path plot (juce::Rectangle<float> r, int n, Fn&& yAtX01)
    {
        juce::Path p;
        for (int i = 0; i <= n; ++i)
        {
            const float t = (float) i / (float) n;
            const float y = yAtX01 (t);
            const float x = r.getX() + t * r.getWidth();
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        return p;
    }

    void fillUnder (juce::Graphics& g, const juce::Path& curve, juce::Rectangle<float> r, juce::Colour c, float baseY)
    {
        juce::Path f (curve);
        f.lineTo (r.getRight(), baseY); f.lineTo (r.getX(), baseY); f.closeSubPath();
        g.setColour (c);
        g.fillPath (f);
    }

    void node (juce::Graphics& g, float x, float y, juce::Colour c, bool on = true, float rad = 5.5f)
    {
        g.setColour (juce::Colour (0xFF15171A));
        g.fillEllipse (x - rad - 1.5f, y - rad - 1.5f, (rad + 1.5f) * 2.0f, (rad + 1.5f) * 2.0f);
        g.setColour (on ? c : kDim);
        g.fillEllipse (x - rad, y - rad, rad * 2.0f, rad * 2.0f);
    }

    juce::Path wave (const VizTaps& t, juce::Rectangle<float> r, float gain, bool side = false)
    {
        const auto& ring = side ? t.scopeSide : t.scopeMid;
        const int w = t.scopeW.load();
        const int pts = juce::jlimit (32, 400, (int) r.getWidth());
        return plot (r, pts, [&] (float x01)
        {
            const int idx = (w + (int) (x01 * (VizTaps::kScope - 1))) & (VizTaps::kScope - 1);
            return r.getCentreY() - juce::jlimit (-1.0f, 1.0f, ring[(size_t) idx] * gain) * r.getHeight() * 0.46f;
        });
    }

    float autoGain (const GridEffect* fx, float target)
    {
        if (fx == nullptr) return 1.0f;
        return juce::jlimit (0.5f, 8.0f, target / juce::jmax (0.01f, dbToGain (fx->levelDb())));
    }

    void ghostWave (juce::Graphics& g, const State& s, juce::Rectangle<float> r, float alpha = 0.18f)
    {
        if (s.fx == nullptr) return;
        g.setColour (s.accent.withAlpha (alpha));
        g.strokePath (wave (s.fx->taps(), r, autoGain (s.fx, 0.5f)), juce::PathStrokeType (1.0f));
    }

    void meterBar (juce::Graphics& g, juce::Rectangle<float> r, float t01, juce::Colour c, bool fromTop = false)
    {
        g.setColour (kGrid);
        g.fillRoundedRectangle (r, 2.0f);
        t01 = clamp01 (t01);
        g.setColour (c);
        if (fromTop) g.fillRoundedRectangle (r.withHeight (r.getHeight() * t01), 2.0f);
        else g.fillRoundedRectangle (r.withTop (r.getBottom() - r.getHeight() * t01), 2.0f);
    }

    juce::String noteStr (float midi, bool flats = false) { const int m = juce::roundToInt (midi); return noteName (m, flats) + juce::String (m / 12 - 1); }

    void spectrumBehind (juce::Graphics& g, const State& s, juce::Rectangle<float> r, float lo, float hi)
    {
        if (s.fx == nullptr) return;
        const auto& t = s.fx->taps();
        const int N = VizTaps::kSpec;
        // the spectrum sits under the curve, scaled so -72..0 dBFS fills the plot
        auto path = plot (r, N - 1, [&] (float x)
        {
            const float db = t.spec[(size_t) juce::jmin (N - 1, (int) std::lround (x * (N - 1)))].load();
            return yOfDb (r, lo + (hi - lo) * clamp01 ((db + 72.0f) / 72.0f), lo, hi);
        });
        fillUnder (g, path, r, juce::Colours::white.withAlpha (0.06f), r.getBottom());
    }

    // ================================================================ analysis
    void drawSpectrum (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        freqGrid (g, r, s.detailed);
        if (s.fx == nullptr) return;
        const float range = s.P (0), top = 0.0f, bot = -range;
        const auto& t = s.fx->taps();
        const int N = VizTaps::kSpec;
        if (s.mode == 0)
        {
            const float bw = r.getWidth() / (float) N;
            g.setColour (s.accent.withAlpha (0.85f));
            for (int b = 0; b < N; ++b)
            {
                const float y = yOfDb (r, t.spec[(size_t) b].load(), bot, top);
                g.fillRect (r.getX() + (float) b * bw + 0.5f, y, juce::jmax (1.0f, bw - 1.0f), r.getBottom() - y);
            }
        }
        else
        {
            auto curve = plot (r, N - 1, [&] (float x) { return yOfDb (r, t.spec[(size_t) juce::jmin (N - 1, (int) std::lround (x * (N - 1)))].load(), bot, top); });
            fillUnder (g, curve, r, s.accent.withAlpha (0.18f), r.getBottom());
            g.setColour (s.accent);
            g.strokePath (curve, juce::PathStrokeType (stroke (s)));
        }
        if (s.I (3) == 1)
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            const float bw = r.getWidth() / (float) N;
            for (int b = 0; b < N; ++b)
                g.fillRect (r.getX() + (float) b * bw, yOfDb (r, t.specPeak[(size_t) b].load(), bot, top), juce::jmax (1.0f, bw - 1.0f), 1.0f);
        }
        if (s.detailed)
        {
            g.setColour (kDim); g.setFont (font (9.0f, false));
            for (int k = 1; k < 4; ++k)
                g.drawText (juce::String (juce::roundToInt (-range * (float) k / 4.0f)) + " dB",
                            juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + r.getHeight() * (float) k / 4.0f - 12.0f, 60.0f, 11.0f), juce::Justification::left);
        }
    }

    void drawScope (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 10, 4);
        g.setColour (kAxis); g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
        if (s.fx == nullptr || s.fx->taps().longScope.empty()) return;
        const auto& t = s.fx->taps();
        const int len = VizTaps::kLongScope;
        const int span = juce::jlimit (16, len / 2, (int) (s.P (0) * 0.001f * (float) s.sr));
        const int w = t.longW.load();
        float gain = dbToGain (s.P (1));
        if (s.I (3) == 1) gain *= autoGain (s.fx, 0.6f);
        int start = (w - span - 1 + len) & (len - 1);
        if (s.mode == 1)
        {
            const float lvl = s.P (2) / juce::jmax (0.001f, gain);
            for (int back = span; back < len / 2; ++back)
            {
                const int i = (w - back + len) & (len - 1), j = (i + 1) & (len - 1);
                if (t.longScope[(size_t) i] <= lvl && t.longScope[(size_t) j] > lvl) { start = i; break; }
            }
            g.setColour (kAmber);
            g.fillRect (r.getX(), r.getCentreY() - juce::jlimit (-1.0f, 1.0f, s.P (2)) * r.getHeight() * 0.46f, 8.0f, 1.0f);
        }
        const int pts = juce::jlimit (32, 600, (int) r.getWidth());
        auto path = plot (r, pts, [&] (float x01)
        {
            const int idx = (start + (int) (x01 * (float) span)) & (len - 1);
            return r.getCentreY() - juce::jlimit (-1.0f, 1.0f, t.longScope[(size_t) idx] * gain) * r.getHeight() * 0.46f;
        });
        g.setColour (s.accent);
        g.strokePath (path, juce::PathStrokeType (stroke (s) * 0.8f));
    }

    void drawLoudness (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        if (s.fx == nullptr) return;
        const auto& t = s.fx->taps();
        const float target = s.P (0), scale = s.I (1) == 0 ? 9.0f : 18.0f;
        const float lo = target - scale * 2.0f, hi = target + scale;
        const float vals[3] = { t.lufsM.load(), t.lufsS.load(), t.lufsI.load() };
        const char* names[3] = { "M", "S", "I" };
        const float colW = r.getWidth() / 3.0f;
        for (int i = 0; i < 3; ++i)
        {
            auto col = juce::Rectangle<float> (r.getX() + colW * (float) i, r.getY(), colW, r.getHeight()).reduced (colW * 0.22f, 6.0f);
            if (s.detailed) col.removeFromBottom (14.0f);
            const bool emph = i == s.mode;
            meterBar (g, col, (vals[i] - lo) / (hi - lo), s.accent.withAlpha (emph ? 0.95f : 0.45f));
            if (s.detailed)
                label (g, col.withY (col.getBottom()).withHeight (16.0f).expanded (20.0f, 0.0f),
                       juce::String (names[i]) + (vals[i] > -99.0f ? " " + juce::String (vals[i], 1) : " --"), juce::Justification::centred, 10.0f, emph ? kText : kDim);
        }
        const float ty = r.getBottom() - (s.detailed ? 20.0f : 6.0f) - (r.getHeight() - (s.detailed ? 26.0f : 12.0f)) * clamp01 ((target - lo) / (hi - lo));
        g.setColour (kAmber);
        g.fillRect (r.getX() + 4.0f, ty, r.getWidth() - 8.0f, 1.2f);
    }

    // goniometer / Lissajous inside a circle: points never leave the boundary
    void drawGonio (juce::Graphics& g, juce::Rectangle<float> r, const State& s, bool liss, float gainDb, bool autoG, float persist)
    {
        const auto c = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.46f;
        g.setColour (kGrid);
        g.drawEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f, 1.0f);
        g.fillRect (c.x, c.y - rad, 1.0f, rad * 2.0f);
        g.fillRect (c.x - rad, c.y, rad * 2.0f, 1.0f);
        if (s.detailed)
        {
            g.setColour (kDim);
            g.setFont (font (8.5f));
            g.drawText (liss ? "R" : "M", juce::Rectangle<float> (c.x + 3.0f, c.y - rad, 20.0f, 12.0f), juce::Justification::left);
            g.drawText (liss ? "L" : "S", juce::Rectangle<float> (c.x + rad - 14.0f, c.y + 2.0f, 14.0f, 12.0f), juce::Justification::left);
        }
        if (s.fx == nullptr) return;
        const auto& t = s.fx->taps();
        float gain = dbToGain (gainDb);
        if (autoG) gain *= autoGain (s.fx, 0.35f);
        const int w = t.scopeW.load();
        const int pts = VizTaps::kScope;
        for (int i = 0; i < pts; ++i)
        {
            const int idx = (w + i) & (pts - 1);
            const float age = (float) i / (float) pts;
            const float mid = t.scopeMid[(size_t) idx] * gain, side = t.scopeSide[(size_t) idx] * gain;
            float x = liss ? (mid + side) : side, y = liss ? (mid - side) : mid;
            const float mag = std::sqrt (x * x + y * y);
            if (mag > 1.0f) { x /= mag; y /= mag; }
            g.setColour (s.accent.withAlpha (0.12f + 0.6f * std::pow (age, 1.0f + 3.0f * (1.0f - persist))));
            g.fillRect (c.x + x * rad - 0.8f, c.y - y * rad - 0.8f, 1.6f, 1.6f);
        }
    }

    void corrBar (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float corr)
    {
        auto bar = r.removeFromBottom (s.detailed ? 16.0f : 8.0f).reduced (r.getWidth() * 0.15f, s.detailed ? 5.0f : 2.0f);
        g.setColour (kGrid); g.fillRoundedRectangle (bar, 2.0f);
        const float x = bar.getCentreX() + juce::jlimit (-1.0f, 1.0f, corr) * bar.getWidth() * 0.5f;
        g.setColour (corr < 0.0f ? kRed : s.accent);
        g.fillRect (juce::jmin (x, bar.getCentreX()), bar.getY(), std::abs (x - bar.getCentreX()), bar.getHeight());
        g.setColour (kText); g.fillRect (bar.getCentreX(), bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
        if (s.detailed)
        {
            g.setColour (kDim); g.setFont (font (8.5f));
            g.drawText ("-1", bar.withX (bar.getX() - 22.0f).withWidth (18.0f), juce::Justification::right);
            g.drawText ("+1", bar.withX (bar.getRight() + 4.0f).withWidth (18.0f), juce::Justification::left);
        }
    }

    void drawPitchDet (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 8, 4);
        if (s.fx == nullptr) return;
        const float hz = s.fx->taps().pitchHz.load();
        if (hz <= 0.0f) { label (g, r, "--", juce::Justification::centred, s.detailed ? 24.0f : 16.0f, kDim); return; }
        const float midi = 69.0f + 12.0f * std::log2 (hz / s.P (0));
        const float cents = (midi - std::round (midi)) * 100.0f;
        ghostWave (g, s, r, 0.12f);
        const float cx = r.getCentreX(), nx = cx + juce::jlimit (-50.0f, 50.0f, cents) / 50.0f * r.getWidth() * 0.45f;
        g.setColour (kAxis); g.fillRect (cx - 0.5f, r.getY(), 1.0f, r.getHeight());
        g.setColour (std::abs (cents) < 5.0f ? juce::Colour (0xFFA9D58E) : s.accent);
        g.fillRect (nx - 1.0f, r.getY(), 2.0f, r.getHeight());
        label (g, r, s.mode == 0 ? noteStr (midi) : juce::String (hz, 1) + " Hz", juce::Justification::centredLeft, s.detailed ? 30.0f : 17.0f, s.accent);
        if (s.detailed) label (g, r, (cents >= 0 ? "+" : "") + juce::String (juce::roundToInt (cents)) + " ct", juce::Justification::centredRight, 13.0f);
    }


    void drawChord (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        const bool flats = s.I (4) == 1;
        const int root = s.fx != nullptr ? (int) s.m (0) : -1;
        const int q = juce::jlimit (0, kNumChordQualities - 1, (int) s.m (1));
        auto bars = r.withTrimmedTop (r.getHeight() * (s.detailed ? 0.45f : 0.5f));
        const float bw = bars.getWidth() / 12.0f;
        static constexpr bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
        for (int k = 0; k < 12; ++k)
        {
            const float v = s.fx != nullptr ? clamp01 (s.fx->taps().spec[(size_t) k].load()) : 0.0f;
            bool inChord = false;
            if (root >= 0) for (int j = 0; j < 6; ++j) if (kChordQualities[q].iv[j] >= 0 && (root + kChordQualities[q].iv[j]) % 12 == k) inChord = true;
            auto col = juce::Rectangle<float> (bars.getX() + bw * (float) k, bars.getY(), bw, bars.getHeight()).reduced (2.0f, 0.0f);
            if (s.detailed) col.removeFromBottom (14.0f);
            g.setColour (black[k] ? juce::Colour (0xFF15171A) : kGrid);
            g.fillRoundedRectangle (col, 2.0f);
            g.setColour (inChord ? s.accent : s.accent.withAlpha (0.35f));
            g.fillRoundedRectangle (col.withTop (col.getBottom() - col.getHeight() * v), 2.0f);
            if (s.detailed) label (g, juce::Rectangle<float> (col.getX() - 6.0f, col.getBottom() - 2.0f, col.getWidth() + 12.0f, 18.0f), noteName (k, flats), juce::Justification::centred, 9.0f, inChord ? kText : kDim);
        }
        auto top = r.withHeight (r.getHeight() - bars.getHeight());
        if (root < 0) { label (g, top, "--", juce::Justification::centred, s.detailed ? 30.0f : 16.0f, kDim); return; }
        if (s.mode == 0)
            label (g, top, noteName (root, flats) + " " + chordQualityName (q), juce::Justification::centred, s.detailed ? 34.0f : 17.0f, s.accent);
        else
        {
            juce::String notes;
            for (int j = 0; j < 6; ++j) if (kChordQualities[q].iv[j] >= 0) notes << noteName ((root + kChordQualities[q].iv[j]) % 12, flats) << "  ";
            label (g, top, notes.trim(), juce::Justification::centred, s.detailed ? 28.0f : 15.0f, s.accent);
        }
    }

    // ================================================================ dynamics
    void drawTransferDb (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 4, 4);
        const float lo = kDynLo, hi = kDynHi;
        g.setColour (kAxis);
        g.drawLine (r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);
        if (s.fx == nullptr) return;
        auto curve = plot (r, juce::jmax (24, (int) r.getWidth() / 2), [&] (float x01)
        {
            float y = 0.0f;
            const float x = lo + (hi - lo) * x01;
            if (! s.fx->transfer (x, s.p, s.mode, s.data, y)) y = x;
            return yOfDb (r, y, lo, hi);
        });
        g.setColour (s.accent);
        g.strokePath (curve, juce::PathStrokeType (stroke (s)));
        const float det = s.m (1);
        if (det > lo)
        {
            float y = det; s.fx->transfer (det, s.p, s.mode, s.data, y);
            g.setColour (juce::Colours::white);
            const float px = r.getX() + (juce::jmin (det, hi) - lo) / (hi - lo) * r.getWidth();
            g.fillEllipse (px - 3.0f, yOfDb (r, y, lo, hi) - 3.0f, 6.0f, 6.0f);
        }
        if (s.detailed && s.d->viz != VizSquash)
        {
            const float th = s.d->viz == VizLimit ? s.P (1) : s.P (0);
            float y = th; s.fx->transfer (th - (s.d->viz == VizLimit ? s.P (0) : 0.0f), s.p, s.mode, s.data, y);
            node (g, r.getX() + (th - lo) / (hi - lo) * r.getWidth(), yOfDb (r, s.d->viz == VizLimit ? th + s.P (5) : y, lo, hi), s.accent);
        }
        const float gr = juce::jmin (s.m (0), s.m (2));
        auto bar = r.removeFromRight (s.detailed ? 10.0f : 6.0f).reduced (1.0f, 2.0f);
        meterBar (g, bar, std::abs (gr) / 24.0f, gr <= 0.0f ? kRed : s.accent, gr <= 0.0f);
        if (s.detailed)
        {
            g.setColour (kDim); g.setFont (font (9.0f, false));
            g.drawText ("IN dB", r.reduced (6.0f, 3.0f), juce::Justification::bottomRight);
            g.drawText ("OUT", r.reduced (6.0f, 3.0f), juce::Justification::topLeft);
        }
    }

    // multiband up/down compressor: per band the input level, up / down thresholds (handles) and the gain
    void drawOtt (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        static const char* const names[3] = { "LOW", "MID", "HIGH" };
        const float lo = -80.0f, hi = 0.0f;
        const float cw = r.getWidth() / 3.0f;
        for (int b = 0; b < 3; ++b)
        {
            auto col = juce::Rectangle<float> (r.getX() + cw * (float) b, r.getY(), cw, r.getHeight()).reduced (s.detailed ? 14.0f : 4.0f, 2.0f);
            if (s.detailed) col.removeFromBottom (14.0f);
            const float dT = s.data[b * 4], uT = s.data[b * 4 + 1];
            g.setColour (kGrid);
            g.fillRoundedRectangle (col, 3.0f);
            g.setColour (kRed.withAlpha (0.12f));
            g.fillRect (col.withBottom (yOfDb (col, dT, lo, hi)));
            g.setColour (s.accent.withAlpha (0.12f));
            g.fillRect (col.withTop (yOfDb (col, uT, lo, hi)));
            const float lv = s.m (3 + b);
            const float ly = yOfDb (col, lv, lo, hi);
            g.setColour (kText.withAlpha (0.55f));
            g.fillRect (col.withTop (ly).reduced (col.getWidth() * 0.32f, 0.0f));
            const float gdb = s.m (b);
            const float oy = yOfDb (col, lv + gdb, lo, hi);
            g.setColour (gdb < 0.0f ? kRed : s.accent);
            g.fillRect (col.getCentreX() - col.getWidth() * 0.18f, juce::jmin (ly, oy), col.getWidth() * 0.36f, std::abs (oy - ly) + 1.0f);
            for (int k = 0; k < 2; ++k)
            {
                const float y = yOfDb (col, k == 0 ? dT : uT, lo, hi);
                g.setColour (k == 0 ? kRed : s.accent);
                g.fillRect (col.getX(), y - 0.75f, col.getWidth(), 1.5f);
                if (s.detailed) node (g, col.getRight() - 10.0f, y, k == 0 ? kRed : s.accent, true, 5.0f);
            }
            if (s.detailed)
            {
                label (g, juce::Rectangle<float> (col.getX(), col.getBottom(), col.getWidth(), 16.0f), names[b], juce::Justification::centred, 9.5f, kDim);
                label (g, col.withHeight (18.0f), (gdb >= 0 ? "+" : "") + juce::String (gdb, 1) + " dB", juce::Justification::centred, 9.5f, kText);
            }
        }
    }

    void drawBands (juce::Graphics& g, juce::Rectangle<float> r, const State& s, int nb, float range, bool bipolar, int meterOffset = 0)
    {
        grid (g, r, juce::jmin (nb, 16), 4);
        if (bipolar) { g.setColour (kAxis); g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f); }
        const float bw = r.getWidth() / (float) nb;
        for (int b = 0; b < nb; ++b)
        {
            const float gdb = s.m (meterOffset + b);
            auto col = juce::Rectangle<float> (r.getX() + bw * (float) b, r.getY(), bw, r.getHeight()).reduced (juce::jmin (5.0f, bw * 0.18f), 2.0f);
            if (bipolar)
            {
                const float h = juce::jlimit (-1.0f, 1.0f, gdb / range) * col.getHeight() * 0.5f;
                g.setColour (gdb < 0.0f ? kRed : s.accent);
                g.fillRect (col.getX(), h > 0 ? col.getCentreY() - h : col.getCentreY(), col.getWidth(), std::abs (h));
            }
            else meterBar (g, col, -gdb / range, kRed, true);
        }
    }

    // denoiser: per band level, the tracked noise floor (+ Sens) and the reduction
    void drawDenoise (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        constexpr int NB = 24;
        const float lo = -100.0f, hi = -10.0f;
        grid (g, r, 8, 4);
        const float bw = r.getWidth() / (float) NB;
        juce::Path floorPath;
        for (int b = 0; b < NB; ++b)
        {
            auto col = juce::Rectangle<float> (r.getX() + bw * (float) b, r.getY(), bw, r.getHeight()).reduced (1.0f, 0.0f);
            const float lv = s.m (24 + b), gr = s.m (b), fl = s.m (48 + b);
            const float y = yOfDb (col, lv, lo, hi);
            g.setColour (s.accent.withAlpha (0.55f));
            g.fillRect (col.withTop (y));
            g.setColour (kRed.withAlpha (0.8f));
            g.fillRect (col.withTop (y).withHeight (juce::jmin (col.getBottom() - y, -gr / (hi - lo) * col.getHeight())));
            const float fy = yOfDb (col, fl + s.P (1), lo, hi);
            if (b == 0) floorPath.startNewSubPath (col.getX(), fy); else floorPath.lineTo (col.getCentreX(), fy);
        }
        floorPath.lineTo (r.getRight(), yOfDb (r, s.m (48 + NB - 1) + s.P (1), lo, hi));
        g.setColour (kAmber);
        g.strokePath (floorPath, juce::PathStrokeType (1.5f));
        if (s.detailed) label (g, r, "noise floor + sens", juce::Justification::topRight, 9.5f, kAmber);
    }

    // ================================================================ EQ / filters
    void drawResponse (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float lo, float hi, float liveHz = 0.0f, bool fillToFloor = false)
    {
        freqGrid (g, r, s.detailed);
        dbGrid (g, r, lo, hi, s.detailed);
        if (s.fx == nullptr) return;
        auto curve = plot (r, juce::jmax (48, (int) r.getWidth() / 2), [&] (float x01)
        {
            float db = 0.0f;
            s.fx->responseDb (hzOfX (r, r.getX() + x01 * r.getWidth()), s.p, s.mode, s.data, db);
            return yOfDb (r, db, lo, hi);
        });
        fillUnder (g, curve, r, s.accent.withAlpha (0.14f), fillToFloor ? r.getBottom() : yOfDb (r, 0.0f, lo, hi));
        g.setColour (s.accent);
        g.strokePath (curve, juce::PathStrokeType (stroke (s)));
        if (liveHz > 0.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.fillRect (xOfHz (r, liveHz) - 0.5f, r.getY(), 1.0f, r.getHeight());
        }
    }

    float responseAt (const State& s, float hz)
    {
        float db = 0.0f;
        if (s.fx != nullptr) s.fx->responseDb (hz, s.p, s.mode, s.data, db);
        return db;
    }

    void drawEq3 (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        float lo, hi; responseRange (VizEq3, lo, hi);
        spectrumBehind (g, s, r, lo, hi);
        drawResponse (g, r, s, lo, hi);
        if (! s.detailed) return;
        static const char* const tags[3] = { "L", "M", "H" };
        for (int b = 0; b < 3; ++b)
        {
            const float f = s.P (3 + b);
            const int type = s.I (8 + b);
            const bool noGain = (b == 0 && type == 0) || (b == 2 && type == 1) || (b == 1 && type != 0);
            const float y = noGain ? yOfDb (r, responseAt (s, f), lo, hi) : yOfDb (r, s.P (b), lo, hi);
            node (g, xOfHz (r, f), y, s.accent);
            g.setColour (juce::Colour (0xFF15171A)); g.setFont (font (8.5f));
            g.drawText (tags[b], juce::Rectangle<float> (xOfHz (r, f) - 6.0f, y - 6.0f, 12.0f, 12.0f), juce::Justification::centred);
        }
    }

    void drawEq8 (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        float lo, hi; responseRange (VizEq8, lo, hi);
        if (s.I (2) == 1) spectrumBehind (g, s, r, lo, hi);
        drawResponse (g, r, s, lo, hi);
        if (! s.detailed) return;
        for (int b = 0; b < Shape::kEqBands; ++b)
        {
            const float* x = s.data + b * Shape::kEqStride;
            const int type = (int) std::lround (x[1]);
            const float y = type <= 2 ? yOfDb (r, x[3] * s.P (1), lo, hi) : yOfDb (r, 0.0f, lo, hi);
            node (g, xOfHz (r, x[2]), y, s.accent, x[0] > 0.5f);
            g.setColour (juce::Colour (0xFF15171A)); g.setFont (font (8.5f));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (xOfHz (r, x[2]) - 6.0f, y - 6.0f, 12.0f, 12.0f), juce::Justification::centred);
        }
    }

    void drawFilterNode (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float lo, float hi, int cutIdx)
    {
        if (! s.detailed) return;
        const float f = s.P (cutIdx);
        node (g, xOfHz (r, f), yOfDb (r, responseAt (s, f), lo, hi), s.accent);
    }

    void drawWavetableFilter (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        float lo, hi; responseRange (VizWavetable, lo, hi);
        drawResponse (g, r, s, lo, hi, 0.0f, true);
        const auto* t = s.table != nullptr ? s.table : Wavetable::builtIn().get();
        const float pos = s.fx != nullptr ? s.m (0) : s.P (0);
        auto fr = r.withTrimmedLeft (r.getWidth() * (s.detailed ? 0.68f : 0.6f)).withHeight (r.getHeight() * 0.42f).reduced (6.0f);
        g.setColour (juce::Colour (0xEE14161A)); g.fillRoundedRectangle (fr, 4.0f);
        g.setColour (kAxis); g.drawRoundedRectangle (fr, 4.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.strokePath (plot (fr, 80, [&] (float x) { return fr.getCentreY() - t->sample (pos, x) * fr.getHeight() * 0.42f; }), juce::PathStrokeType (1.2f));
    }

    void drawAllpass (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        freqGrid (g, r, s.detailed);
        const int stages = juce::jlimit (1, 64, s.I (2));
        const float centre = s.fx != nullptr && s.m (0) > 0.0f ? s.m (0) : s.P (0);
        auto curve = plot (r, juce::jmax (48, (int) r.getWidth() / 2), [&] (float x01)
        {
            const float f = hzOfX (r, r.getX() + x01 * r.getWidth());
            float gd = 0.0f;
            for (int k = 0; k < stages; ++k)
            {
                const float t = stages > 1 ? (float) k / (float) (stages - 1) - 0.5f : 0.0f;
                const float fc = centre * std::pow (2.0f, t * s.P (3));
                const float u = std::log2 (f / fc) / (0.5f / juce::jmax (0.2f, s.P (1)));
                gd += 1.0f / (1.0f + u * u);
            }
            return r.getBottom() - clamp01 (gd / std::sqrt ((float) stages) * 0.6f) * r.getHeight() * 0.9f;
        });
        fillUnder (g, curve, r, s.accent.withAlpha (0.15f), r.getBottom());
        g.setColour (s.accent);
        g.strokePath (curve, juce::PathStrokeType (stroke (s)));
        if (s.detailed) node (g, xOfHz (r, s.P (0)), r.getCentreY(), s.accent);
    }

    // ================================================================ volume
    void drawTransient (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        // the gain change one hit gets: the attack bump fading over Decay into
        // the sustain change, held over the Release tail
        auto bar = r.removeFromRight (s.detailed ? 10.0f : 6.0f).reduced (1.0f, 2.0f);
        const float span = transientSpanMs (s);
        timeGrid (g, r, s, span);
        ghostWave (g, s, r, 0.35f);
        const float y0 = yOfDb (r, 0.0f, -kTsRangeDb, kTsRangeDb);
        g.setColour (kAxis);
        g.drawHorizontalLine (juce::roundToInt (y0), r.getX(), r.getRight());
        auto path = plot (r, juce::jmax (64, (int) r.getWidth() / 2), [&] (float x01)
        {
            return yOfDb (r, transientCurveDb (s, x01 * span), -kTsRangeDb, kTsRangeDb);
        });
        juce::Path fillP (path);
        fillP.lineTo (r.getRight(), y0); fillP.lineTo (r.getX(), y0); fillP.closeSubPath();
        g.setColour (s.accent.withAlpha (0.16f)); g.fillPath (fillP);
        g.setColour (s.accent);
        g.strokePath (path, juce::PathStrokeType (stroke (s)));
        if (s.detailed)
        {
            const float dec = s.P (6), rel = s.P (5);
            auto X = [&] (float ms) { return xOfMs (r, ms, span); };
            auto Y = [&] (float db) { return yOfDb (r, db, -kTsRangeDb, kTsRangeDb); };
            node (g, X (span * 0.01f), Y (s.P (0) * 12.0f), s.accent);
            node (g, X (dec), Y (transientCurveDb (s, dec)), s.accent, false);
            node (g, X (dec + rel * 0.5f), Y (s.P (1) * 12.0f), s.accent);
            node (g, X (dec + rel), Y (s.P (1) * 12.0f), s.accent, false);
            g.setFont (font (10.0f));
            g.setColour (kText);
            g.drawText ("decay " + juce::String (juce::roundToInt (dec)) + " ms", juce::Rectangle<float> (X (dec) + 6.0f, Y (transientCurveDb (s, dec)) - 16.0f, 110.0f, 12.0f), juce::Justification::centredLeft, false);
            g.drawText ("release " + juce::String (juce::roundToInt (rel)) + " ms", juce::Rectangle<float> (X (dec + rel) - 116.0f, Y (s.P (1) * 12.0f) - 16.0f, 110.0f, 12.0f), juce::Justification::centredRight, false);
        }
        // live gain
        const float gdb = s.m (0);
        g.setColour (kGrid); g.fillRoundedRectangle (bar, 2.0f);
        const float h = juce::jlimit (-1.0f, 1.0f, gdb / 18.0f) * bar.getHeight() * 0.5f;
        g.setColour (gdb >= 0.0f ? s.accent : kRed);
        g.fillRect (bar.getX(), h > 0 ? bar.getCentreY() - h : bar.getCentreY(), bar.getWidth(), std::abs (h));
    }

    void drawMbGate (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        // five columns split at the crossovers; in each, the band's level, its
        // threshold line and (when closed) the drop to its floor
        grid (g, r, 1, 4);
        const float offs = s.P (4), depth = s.P (5);
        auto Y = [&] (float db) { return yOfDb (r, db, kMbgLo, kMbgHi); };
        for (int b = 0; b < Shape::kMbgBands; ++b)
        {
            const auto xr = mbgBandX (r, s, b);
            auto col = juce::Rectangle<float> (xr.getStart(), r.getY(), xr.getLength(), r.getHeight()).reduced (1.5f, 0.0f);
            const float* d = s.data + b * Shape::kMbgStride;
            const bool on = d[2] > 0.5f;
            const auto tint = s.accent.withRotatedHue (0.06f * (float) (b - 2));
            g.setColour (tint.withAlpha (on ? 0.07f : 0.02f));
            g.fillRect (col);
            // live level and gate gain
            if (s.fx != nullptr)
            {
                const float lvl = juce::jlimit (kMbgLo, kMbgHi, s.m (b)), gdb = s.m (Shape::kMbgBands + b);
                const bool open = gdb > -1.0f;
                auto bar = col.withTrimmedTop (Y (lvl) - col.getY()).reduced (col.getWidth() * 0.3f, 0.0f);
                g.setColour (on ? (open ? tint.withAlpha (0.75f) : tint.withAlpha (0.25f)) : kDim.withAlpha (0.3f));
                g.fillRect (bar);
                if (s.detailed && on && ! open)
                {
                    g.setColour (kRed.withAlpha (0.8f));
                    g.setFont (font (10.0f));
                    g.drawText (juce::String (juce::roundToInt (gdb)) + " dB", col.withTrimmedTop (col.getHeight() - 20.0f), juce::Justification::centred, false);
                }
            }
            if (! on)
            {
                g.setColour (kDim);
                g.setFont (font (s.detailed ? 11.0f : 9.0f));
                g.drawText ("OFF", col, juce::Justification::centred, false);
                continue;
            }
            // threshold, and the range under it (where a closed band drops to)
            const float thY = Y (d[0] + offs);
            const float floorDb = d[1] * depth;
            g.setColour (tint.withAlpha (0.12f));
            g.fillRect (col.withTop (thY));
            g.setColour (tint);
            g.drawHorizontalLine (juce::roundToInt (thY), col.getX(), col.getRight());
            if (s.detailed)
            {
                g.setColour (kText);
                g.setFont (font (10.0f));
                const juce::String rng = floorDb <= Shape::kMbgFloorDb + 0.1f ? juce::String ("mute") : juce::String (juce::roundToInt (floorDb)) + " dB";
                g.drawText (juce::String (juce::roundToInt (d[0] + offs)) + " dB  /  " + rng, juce::Rectangle<float> (col.getX(), thY - 16.0f, col.getWidth(), 14.0f),
                            juce::Justification::centred, false);
                node (g, col.getCentreX(), thY, tint);
            }
        }
        // crossovers
        for (int k = 0; k < Shape::kMbgBands - 1; ++k)
        {
            const float x = xOfHz (r, Shape::mbgCrossover (s.p, k));
            g.setColour (kAxis);
            g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());
            if (s.detailed)
            {
                node (g, x, r.getY() + 10.0f, s.accent, false);
                const float f = Shape::mbgCrossover (s.p, k);
                g.setColour (kDim);
                g.setFont (font (9.5f));
                g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 1) + "k" : juce::String (juce::roundToInt (f)), juce::Rectangle<float> (x + 5.0f, r.getY() + 3.0f, 44.0f, 14.0f), juce::Justification::centredLeft, false);
            }
        }
    }

    void drawHarmRes (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        float hz[harmres::kMaxRes], gain[harmres::kMaxRes];
        const int n = harmres::notes (s.p, s.mode, s.data, hz, gain);
        const float q = harmres::qOf (s.P (5));
        auto cr = harmResCurve (r, s.detailed);
        grid (g, cr, 1, 4);
        for (float f : { 100.0f, 1000.0f, 10000.0f })
        {
            g.setColour (kGrid);
            g.drawVerticalLine (juce::roundToInt (xOfHz (cr, f)), cr.getY(), cr.getBottom());
        }
        // outside Low..High nothing rings
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (cr.withRight (xOfHz (cr, s.P (3))));
        g.fillRect (cr.withLeft (xOfHz (cr, s.P (4))));
        // resonances as thin peaks (dB, top = 0 dB, floor = -36 dB)
        auto path = plot (cr, juce::jmax (96, (int) cr.getWidth()), [&] (float x01)
        {
            const float f = kFLo * std::pow (kFHi / kFLo, x01);
            const float db = juce::jlimit (-36.0f, 6.0f, 20.0f * std::log10 (juce::jmax (1.0e-5f, harmres::magnitude (f, hz, gain, n, q))));
            return cr.getBottom() - (db + 36.0f) / 42.0f * cr.getHeight();
        });
        fillUnder (g, path, cr, s.accent.withAlpha (0.18f), cr.getBottom());
        g.setColour (s.accent);
        g.strokePath (path, juce::PathStrokeType (s.detailed ? 1.4f : 1.1f));
        // keyboard: the pitch set (root ringed)
        float w12[12];
        harmres::pitchSet (s.p, s.mode, s.data, w12);
        const int root = juce::jlimit (0, 11, s.I (0));
        auto keys = harmResKeys (r, s.detailed);
        for (int pass = 0; pass < 2; ++pass)
            for (int pc = 0; pc < 12; ++pc)
            {
                const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
                if ((pass == 0) == black) continue;
                const auto kr = harmResKeyRect (keys, pc).reduced (black ? 0.5f : 1.0f, 0.0f);
                g.setColour (black ? juce::Colour (0xFF15161A) : juce::Colour (0xFF2A2C32));
                g.fillRoundedRectangle (kr, 2.5f);
                if (w12[pc] > 0.001f)
                {
                    g.setColour (s.accent.withAlpha (0.25f + 0.75f * w12[pc]));
                    g.fillRoundedRectangle (kr.withTrimmedTop (kr.getHeight() * (1.0f - w12[pc])), 2.5f);
                }
                if (pc == root && s.mode != harmres::kModeCustom)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.drawRoundedRectangle (kr.reduced (1.0f), 2.5f, 1.2f);
                }
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.drawRoundedRectangle (kr, 2.5f, 0.8f);
            }
        // the arp's current note
        if (s.fx != nullptr && s.I (10) != harmres::kArpOff && s.m (1) >= 0.0f)
        {
            const auto kr = harmResKeyRect (keys, juce::jlimit (0, 11, (int) std::lround (s.m (1))));
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (kr.reduced (1.5f), 2.5f, 2.2f);
        }
        if (s.detailed)
        {
            label (g, cr, harmres::setName (s.p, s.mode) + "   " + juce::String (n) + " resonances"
                         + (s.I (10) != harmres::kArpOff ? juce::String ("   arp ") + s.d->p[10].choices[juce::jlimit (0, 6, s.I (10))] + " " + s.d->p[11].choices[juce::jlimit (0, 14, s.I (11))] : juce::String()),
                  juce::Justification::topLeft, 11.0f, kText);
            node (g, xOfHz (cr, s.P (3)), cr.getBottom() - 10.0f, s.accent);
            node (g, xOfHz (cr, s.P (4)), cr.getBottom() - 10.0f, s.accent);
        }
    }

    void drawDucker (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 4, 4);
        // drawn as the resulting gain: full level at the top, (1 - depth) where fully ducked
        const float depth = juce::jmax (0.1f, s.P (0));
        auto Y = [&] (float amt) { return r.getY() + 3.0f + clamp01 (amt * depth) * (r.getHeight() - 6.0f); };
        auto path = plot (r, juce::jmax (64, (int) r.getWidth() / 2), [&] (float x01) { return Y (clamp01 (CurvePoints::eval (s.data, x01))); });
        juce::Path fillP (path);
        fillP.lineTo (r.getRight(), r.getY()); fillP.lineTo (r.getX(), r.getY()); fillP.closeSubPath();
        g.setColour (s.accent.withAlpha (0.14f)); g.fillPath (fillP);
        g.setColour (s.accent);
        g.strokePath (path, juce::PathStrokeType (stroke (s)));
        if (s.detailed)
        {
            const int n = CurvePoints::count (s.data);
            for (int i = 0; i < n; ++i) node (g, r.getX() + s.data[1 + i * 3] * r.getWidth(), Y (clamp01 (s.data[2 + i * 3])), s.accent, true, 4.5f);
        }
        const float x = r.getX() + clamp01 (s.m (1)) * r.getWidth();
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.fillRect (x, r.getY(), 1.0f, r.getHeight());
    }

    void drawSteps (juce::Graphics& g, juce::Rectangle<float> r, const State& s, int steps, int current, const std::function<float (int)>& level)
    {
        const float bw = r.getWidth() / (float) steps;
        for (int k = 0; k < steps; ++k)
        {
            auto cell = juce::Rectangle<float> (r.getX() + bw * (float) k, r.getY(), bw, r.getHeight()).reduced (bw > 10.0f ? 2.0f : 1.0f, 4.0f);
            const float lv = level (k);
            g.setColour ((k / 4) % 2 == 0 ? kGrid : kGrid.brighter (0.08f));
            g.fillRoundedRectangle (cell, 2.0f);
            if (lv > 0.0f)
            {
                g.setColour (s.accent.withAlpha (k == current ? 1.0f : 0.6f));
                g.fillRoundedRectangle (cell.withTop (cell.getBottom() - cell.getHeight() * lv), 2.0f);
            }
            if (k == current) { g.setColour (juce::Colours::white.withAlpha (0.8f)); g.drawRoundedRectangle (cell, 2.0f, 1.0f); }
        }
    }

    void drawGain (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 8, 4);
        ghostWave (g, s, r, 0.35f);
        if (s.fx == nullptr) return;
        const float lv = clamp01 ((s.fx->peakDb() + 60.0f) / 60.0f);
        auto bar = r.removeFromRight (s.detailed ? 12.0f : 7.0f).reduced (1.0f, 2.0f);
        meterBar (g, bar, lv, lv > 0.98f ? kRed : s.accent);
        if (s.detailed)
        {
            const auto pr = juce::Rectangle<float> (r.getCentreX() - 60.0f, r.getBottom() - 16.0f, 120.0f, 6.0f);
            g.setColour (kGrid); g.fillRoundedRectangle (pr, 3.0f);
            g.setColour (s.accent); g.fillEllipse (pr.getCentreX() + s.P (1) * 60.0f - 5.0f, pr.getCentreY() - 5.0f, 10.0f, 10.0f);
        }
    }

    // ================================================================ modulation
    float lfoDepth (const State& s)
    {
        switch (s.d->viz)
        {
            case VizAutoPan: case VizTremolo: case VizWobble: return juce::jmax (0.15f, s.P (3));
            case VizChorus: case VizFlanger: case VizPhaser: return juce::jmax (0.15f, s.P (1));
            case VizVibrato: return clamp01 (s.P (1) / 200.0f + 0.15f);
            default: return 1.0f;
        }
    }

    void drawLfo (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 8, 4);
        g.setColour (kAxis); g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
        const int si = lfoShapeIndex (*s.d), mi = lfoMorphIndex (*s.d);
        const int shape = si >= 0 ? juce::jlimit (0, LfoShapes::kNum - 1, s.I (si)) : 0;
        const float morph = mi >= 0 ? s.P (mi) : 0.0f;
        const float depth = lfoDepth (s);
        const int cycles = lfoCycles (s.detailed);
        ShapeLfo preview;
        auto Y = [&] (float v) { return r.getCentreY() - v * depth * r.getHeight() * 0.44f; };
        auto valueAt = [&] (double ph)
        {
            // S&H / smooth random previews use a fixed pseudo-random sequence
            const int cyc = (int) std::floor (ph * 4.0);
            preview.held = std::sin ((float) cyc * 2.3f) * 0.8f; preview.next = std::sin ((float) (cyc + 1) * 2.3f) * 0.8f;
            preview.phase = 0.0;
            return preview.value (shape, morph, s.data, ph - std::floor (ph));
        };
        auto path = plot (r, juce::jmax (64, (int) r.getWidth() / 2), [&] (float x01) { return Y (valueAt (x01 * cycles)); });
        g.setColour (s.accent);
        g.strokePath (path, juce::PathStrokeType (stroke (s)));
        if (s.detailed && shape == LfoShapes::kCustom)
        {
            const int n = CurvePoints::count (s.data);
            for (int i = 0; i < n; ++i) node (g, r.getX() + s.data[1 + i * 3] * r.getWidth(), Y (s.data[2 + i * 3]), s.accent, true, 4.5f);
        }
        const float ph = s.m (0);
        for (int k = 0; k < cycles; ++k)
        {
            const float x = r.getX() + ((float) k + ph) / (float) cycles * r.getWidth();
            g.setColour (juce::Colours::white);
            g.fillEllipse (x - 3.5f, Y (valueAt (ph)) - 3.5f, 7.0f, 7.0f);
        }
        if (s.detailed && shape != LfoShapes::kCustom)
            label (g, r, "drag to draw a custom shape", juce::Justification::bottomRight, 9.0f, kDim);
    }

    void drawWah (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        freqGrid (g, r, s.detailed);
        const float lo = s.P (3), hi = s.P (4), q = s.P (5);
        const float fc = s.m (1) > 0.0f ? s.m (1) : lo * std::pow (hi / lo, s.P (0));
        g.setColour (s.accent.withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOfHz (r, lo), r.getY(), xOfHz (r, hi), r.getBottom()));
        auto curve = plot (r, juce::jmax (48, (int) r.getWidth() / 2), [&] (float x01)
        {
            const float f = hzOfX (r, r.getX() + x01 * r.getWidth());
            const float db = Svf::responseDb (2, f, fc, std::sqrt (juce::jmax (0.0f, (q - 0.5f) / 39.5f)));
            return yOfDb (r, db + 20.0f * std::log10 (std::sqrt (q) * 1.1f), -24.0f, 24.0f);
        });
        g.setColour (s.accent);
        g.strokePath (curve, juce::PathStrokeType (stroke (s)));
        if (s.detailed) node (g, xOfHz (r, fc), r.getY() + 12.0f, s.accent);
    }

    void drawDoppler (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 6, 4);
        const auto c = juce::Point<float> (r.getCentreX(), r.getBottom() - r.getHeight() * 0.2f);
        g.setColour (kText);
        g.fillEllipse (c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f);
        const float scale = juce::jmin (r.getWidth(), r.getHeight() * 1.6f) * 0.45f;
        g.setColour (kAxis);
        if (s.mode == 0) g.fillRect (r.getX(), c.y - juce::jlimit (0.0f, 1.0f, s.m (1)) * scale, r.getWidth(), 1.0f);
        g.setColour (s.accent);
        g.fillEllipse (c.x + juce::jlimit (-1.0f, 1.0f, s.m (0)) * scale - 6.0f, c.y - juce::jlimit (-1.0f, 1.0f, s.m (1)) * scale - 6.0f, 12.0f, 12.0f);
    }

    void bigText (juce::Graphics& g, juce::Rectangle<float> r, const State& s, const juce::String& big, const juce::String& small = {})
    {
        grid (g, r, 8, 4);
        ghostWave (g, s, r, 0.15f);
        label (g, r, big, small.isEmpty() ? juce::Justification::centred : juce::Justification::centredLeft, s.detailed ? 28.0f : 16.0f, s.accent);
        if (small.isNotEmpty()) label (g, r, small, juce::Justification::centredRight, s.detailed ? 13.0f : 10.0f, kText);
    }

    // ================================================================ stereo
    // stereo image: a fan whose opening shows the output width (side vs mid
    // energy), the Width setting as an outline, and the correlation
    void drawStereoImage (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float targetWidth)
    {
        auto area = r.withTrimmedBottom (s.detailed ? 18.0f : 8.0f);
        const auto c = juce::Point<float> (area.getCentreX(), area.getBottom() - 4.0f);
        const float rad = juce::jmin (area.getWidth() * 0.48f, area.getHeight() - 8.0f);
        g.setColour (kGrid);
        juce::Path arc; arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, -kPi * 0.5f, kPi * 0.5f, true);
        g.strokePath (arc, juce::PathStrokeType (1.0f));
        g.fillRect (c.x, c.y - rad, 1.0f, rad);
        g.setColour (kDim); g.setFont (font (8.5f));
        g.drawText ("L", juce::Rectangle<float> (c.x - rad - 2.0f, c.y - 12.0f, 12.0f, 12.0f), juce::Justification::left);
        g.drawText ("R", juce::Rectangle<float> (c.x + rad - 10.0f, c.y - 12.0f, 12.0f, 12.0f), juce::Justification::right);
        auto fan = [&] (float width, juce::Colour col, bool filled)
        {
            const float half = juce::jlimit (0.03f, 1.0f, width) * kPi * 0.5f;
            juce::Path p;
            p.startNewSubPath (c);
            p.addCentredArc (c.x, c.y, rad * 0.95f, rad * 0.95f, 0.0f, -half, half, false);
            p.closeSubPath();
            g.setColour (col);
            if (filled) g.fillPath (p); else g.strokePath (p, juce::PathStrokeType (1.2f));
        };
        float wOut = 0.0f;
        if (s.fx != nullptr)
        {
            double mm = 0.0, ss = 0.0;
            const auto& t = s.fx->taps();
            for (int i = 0; i < VizTaps::kScope; ++i) { mm += (double) t.scopeMid[(size_t) i] * t.scopeMid[(size_t) i]; ss += (double) t.scopeSide[(size_t) i] * t.scopeSide[(size_t) i]; }
            wOut = clamp01 ((float) (2.0 * std::sqrt (ss) / juce::jmax (1.0e-9, std::sqrt (mm) + std::sqrt (ss))));
        }
        fan (clamp01 (targetWidth * 0.5f), s.accent.withAlpha (0.6f), false);
        fan (wOut, s.accent.withAlpha (0.35f), true);
        if (s.detailed) label (g, area, "Width " + juce::String (juce::roundToInt (targetWidth * 100.0f)) + "%", juce::Justification::topLeft, 10.0f);
        corrBar (g, r, s, s.m (0));
    }

    void drawHaas (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        auto area = r.withTrimmedBottom (s.detailed ? 18.0f : 8.0f);
        const float spanMs = 45.0f;
        const float laneH = area.getHeight() / 2.0f;
        const bool right = s.mode == 0;
        for (int lane = 0; lane < 2; ++lane)
        {
            auto ln = juce::Rectangle<float> (area.getX(), area.getY() + laneH * (float) lane, area.getWidth(), laneH).reduced (2.0f, 6.0f);
            g.setColour (kGrid); g.fillRoundedRectangle (ln, 3.0f);
            if (s.detailed) label (g, ln, lane == 0 ? "L" : "R", juce::Justification::centredLeft, 10.0f, kDim);
            const bool delayed = (lane == 1) == right;
            const float x0 = ln.getX() + 22.0f;
            const float w = ln.getWidth() - 30.0f;
            // direct sound at 0; the delayed side's highs (above Lowcut) arrive Delay ms later
            g.setColour (s.accent.withAlpha (delayed ? 0.35f : 0.9f));
            g.fillRect (x0, ln.getY() + 3.0f, 3.0f, ln.getHeight() - 6.0f);
            if (delayed)
            {
                const float x = x0 + juce::jmin (1.0f, s.P (0) / spanMs) * w;
                g.setColour (s.accent);
                g.fillRect (x, ln.getY() + 3.0f, 3.0f, (ln.getHeight() - 6.0f) * juce::jmax (0.15f, s.P (1)));
                if (s.detailed)
                {
                    node (g, x + 1.5f, ln.getCentreY(), s.accent);
                    label (g, ln.withX (x + 8.0f).withWidth (180.0f), juce::String (s.P (0), 1) + " ms  (above " + juce::String (juce::roundToInt (s.P (2))) + " Hz)", juce::Justification::centredLeft, 9.5f, kText);
                }
            }
        }
        corrBar (g, r, s, s.m (0));
    }

    void drawMonoBass (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        auto area = r.withTrimmedBottom (s.detailed ? 18.0f : 8.0f);
        freqGrid (g, area, s.detailed);
        const float x = xOfHz (area, s.P (0));
        const float lw = clamp01 (s.P (1)), hw = juce::jlimit (0.0f, 2.0f, s.P (3));
        auto region = [&] (float x0, float x1, float width, bool muted)
        {
            const float h = juce::jmax (2.0f, width / 2.0f * area.getHeight() * 0.8f);
            g.setColour (s.accent.withAlpha (muted ? 0.1f : 0.35f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x0, area.getCentreY() - h * 0.5f, x1, area.getCentreY() + h * 0.5f));
        };
        const int listen = s.I (4);
        region (area.getX(), x, lw, listen == 2);
        region (x, area.getRight(), hw, listen == 1);
        g.setColour (s.accent);
        g.fillRect (x - 1.0f, area.getY(), 2.0f, area.getHeight());
        if (s.detailed)
        {
            node (g, x, area.getY() + 12.0f, s.accent);
            label (g, area.withRight (x), lw < 0.01f ? "MONO" : "W " + juce::String (juce::roundToInt (lw * 100.0f)) + "%", juce::Justification::centredLeft, 10.0f);
            label (g, area.withLeft (x), "W " + juce::String (juce::roundToInt (hw * 100.0f)) + "%", juce::Justification::centredRight, 10.0f);
        }
        corrBar (g, r, s, s.m (0));
    }

    // ================================================================ pitch
    void drawShiftArrow (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float fromHz, float toHz)
    {
        freqGrid (g, r, s.detailed);
        const float y = r.getCentreY();
        const float x0 = xOfHz (r, fromHz), x1 = xOfHz (r, toHz);
        g.setColour (kDim); g.fillRect (x0 - 1.0f, r.getY() + 6.0f, 2.0f, r.getHeight() - 12.0f);
        g.setColour (s.accent); g.fillRect (x1 - 1.0f, r.getY() + 6.0f, 2.0f, r.getHeight() - 12.0f);
        juce::Path a; a.addArrow ({ x0, y, x1, y }, 1.5f, 8.0f, 8.0f);
        g.fillPath (a);
    }

    void drawVocoder (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 8, 4);
        const int nb = juce::jlimit (1, 32, (int) s.m (VizTaps::kMeters - 1));
        const float bw = r.getWidth() / (float) nb;
        for (int b = 0; b < nb; ++b)
        {
            const float h = clamp01 (s.m (b)) * r.getHeight();
            g.setColour (s.accent.withAlpha (0.85f));
            g.fillRect (r.getX() + bw * (float) b + 0.5f, r.getBottom() - h, juce::jmax (1.0f, bw - 1.0f), h);
        }
    }

    void drawResonator (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        static constexpr int chords[8][4] = { { 0,0,0,0 }, { 0,12,0,12 }, { 0,7,12,19 }, { 0,4,7,12 }, { 0,3,7,12 }, { 0,5,7,12 }, { 0,4,7,11 }, { 0,3,7,10 } };
        static constexpr bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
        const int root = juce::roundToInt (s.P (0));
        const int ch = juce::jlimit (0, 7, s.I (1));
        const int keys = 25;
        const float kw = r.getWidth() / (float) keys;
        for (int k = 0; k < keys; ++k)
        {
            const bool bl = black[(((root + k) % 12) + 12) % 12];
            bool lit = false;
            for (int n : chords[ch]) if (n == k) lit = true;
            auto key = juce::Rectangle<float> (r.getX() + kw * (float) k, r.getY() + (bl ? 0.0f : r.getHeight() * 0.25f), kw, r.getHeight() * (bl ? 0.6f : 0.75f)).reduced (0.5f, 2.0f);
            g.setColour (lit ? s.accent : (bl ? juce::Colour (0xFF15171A) : juce::Colour (0xFF34373E)));
            g.fillRoundedRectangle (key, 1.5f);
        }
        if (s.detailed) label (g, r, noteStr ((float) root) + "  " + juce::String (s.d->p[1].choices[ch]), juce::Justification::topLeft, 11.0f, kText);
    }

    // ================================================================ drive
    void drawTransferLin (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 4, 4);
        g.setColour (kAxis);
        g.drawLine (r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);
        g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
        g.fillRect (r.getCentreX(), r.getY(), 1.0f, r.getHeight());
        if (s.fx == nullptr) return;
        auto curve = plot (r, juce::jmax (64, (int) r.getWidth()), [&] (float x01)
        {
            float y = 0.0f;
            if (! s.fx->transfer (x01 * 2.0f - 1.0f, s.p, s.mode, s.data, y)) y = x01 * 2.0f - 1.0f;
            return r.getCentreY() - juce::jlimit (-1.1f, 1.1f, y) * r.getHeight() * 0.45f;
        });
        g.setColour (s.accent);
        g.strokePath (curve, juce::PathStrokeType (stroke (s)));
        const float pk = clamp01 (s.m (0));
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.fillRect (r.getCentreX() + pk * r.getWidth() * 0.5f, r.getY(), 1.0f, r.getHeight());
        g.fillRect (r.getCentreX() - pk * r.getWidth() * 0.5f, r.getY(), 1.0f, r.getHeight());
    }

    void drawWaveshaper (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        drawTransferLin (g, r, s);
        if (s.detailed && s.I (0) == 11)
        {
            // the custom curve's points live on the positive half (mirrored for negative input)
            const int n = CurvePoints::count (s.data);
            auto half = r.withTrimmedLeft (r.getWidth() * 0.5f);
            for (int i = 0; i < n; ++i)
                node (g, half.getX() + s.data[1 + i * 3] * half.getWidth(), r.getCentreY() - s.data[2 + i * 3] * r.getHeight() * 0.45f, s.accent, true, 4.5f);
        }
        else if (s.detailed) label (g, r, "drag on the right half to draw a Custom curve", juce::Justification::bottomRight, 9.0f, kDim);
    }

    void drawHarmonicBars (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        const int NH = Shape::kHarmonics;
        const float bw = r.getWidth() / (float) NH;
        for (int h = 0; h < NH; ++h)
        {
            auto col = juce::Rectangle<float> (r.getX() + bw * (float) h, r.getY(), bw, r.getHeight()).reduced (bw * 0.15f, 2.0f);
            if (s.detailed) col.removeFromBottom (14.0f);
            g.setColour (kGrid); g.fillRoundedRectangle (col, 2.0f);
            const bool even = ((h + 2) & 1) == 0;
            g.setColour (even ? s.accent : s.accent.interpolatedWith (juce::Colours::white, 0.35f));
            g.fillRoundedRectangle (col.withTop (col.getBottom() - col.getHeight() * clamp01 (s.data[h])), 2.0f);
            if (s.detailed) label (g, juce::Rectangle<float> (col.getX() - 4.0f, col.getBottom() - 2.0f, col.getWidth() + 8.0f, 18.0f), juce::String (h + 2), juce::Justification::centred, 8.5f, kDim);
        }
        if (s.detailed) label (g, r, "harmonic levels (drag)  -  even / odd", juce::Justification::topRight, 9.0f, kDim);
    }

    void drawMbDrive (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        freqGrid (g, r, s.detailed);
        const float x1 = xOfHz (r, s.P (3)), x2 = xOfHz (r, juce::jmax (s.P (4), s.P (3) * 1.5f));
        const float xs[4] = { r.getX(), x1, x2, r.getRight() };
        static const char* const names[3] = { "LOW", "MID", "HIGH" };
        for (int b = 0; b < 3; ++b)
        {
            auto band = juce::Rectangle<float>::leftTopRightBottom (xs[b] + 2.0f, r.getY() + 2.0f, xs[b + 1] - 2.0f, r.getBottom() - 2.0f);
            const float drive = clamp01 (s.P (b) / 24.0f);
            g.setColour (s.accent.withAlpha (0.12f + 0.35f * drive));
            g.fillRoundedRectangle (band.withTop (band.getBottom() - band.getHeight() * (0.15f + 0.85f * drive)), 3.0f);
            const float pk = clamp01 (s.m (2 + b));
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.fillRect (band.getX(), band.getBottom() - band.getHeight() * pk, band.getWidth(), 1.5f);
            if (s.detailed)
            {
                label (g, band.withHeight (18.0f), names[b], juce::Justification::centred, 9.5f, kDim);
                label (g, band.withTrimmedTop (18.0f).withHeight (18.0f), "+" + juce::String (juce::roundToInt (s.P (b))) + " dB", juce::Justification::centred, 11.0f, kText);
            }
        }
        for (float x : { x1, x2 })
        {
            g.setColour (s.accent);
            g.fillRect (x - 1.0f, r.getY(), 2.0f, r.getHeight());
            if (s.detailed) node (g, x, r.getBottom() - 14.0f, s.accent);
        }
    }

    // ================================================================ space
    // vertical grid at round times, labelled on the panel
    void timeGrid (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float span)
    {
        grid (g, r, 1, 4);
        float step = span / 4.0f;
        if (s.detailed) step = span / 8.0f;
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        for (int i = 1; (float) i * step < span - 0.5f; ++i)
        {
            const float t = (float) i * step, x = xOfMs (r, t, span);
            g.setColour (kGrid);
            g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());
            if (s.detailed && (i % 2 == 0))
            {
                g.setColour (kDim);
                const juce::String txt = t >= 1000.0f ? juce::String (t / 1000.0f, t < 10000.0f && std::fmod (t, 1000.0f) > 1.0f ? 1 : 0) + " s" : juce::String (juce::roundToInt (t)) + " ms";
                g.drawText (txt, juce::Rectangle<float> (x - 40.0f, r.getBottom() - 14.0f, 38.0f, 12.0f), juce::Justification::centredRight, false);
            }
        }
    }

    void drawDecay (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float rt60, float preMs)
    {
        // energy decay in dB: a straight slope from 0 dB that reaches the
        // floor (-60 dB) at RT60, on a fixed time axis
        const float span = decaySpanMs (s);
        timeGrid (g, r, s, span);
        const float rtMs = juce::jmax (50.0f, rt60 * 1000.0f);
        const float x0 = xOfMs (r, preMs, span), x1 = xOfMs (r, preMs + rtMs, span);
        juce::Path path;
        path.startNewSubPath (r.getX(), r.getBottom() - 2.0f);
        path.lineTo (x0, r.getBottom() - 2.0f);
        path.lineTo (x0, decayYOfDb (r, 0.0f));
        path.lineTo (juce::jmin (x1, r.getRight()), decayYOfDb (r, -60.0f * juce::jmin (1.0f, (r.getRight() - x0) / juce::jmax (1.0f, x1 - x0))));
        if (x1 < r.getRight()) path.lineTo (r.getRight(), r.getBottom() - 2.0f);
        fillUnder (g, path, r, s.accent.withAlpha (0.16f), r.getBottom());
        g.setColour (s.accent);
        g.strokePath (path, juce::PathStrokeType (stroke (s)));
        if (s.detailed)
        {
            // pre-delay bracket and RT60 marker
            g.setColour (s.accent.withAlpha (0.55f));
            const float dash[] = { 3.0f, 3.0f };
            if (x1 <= r.getRight()) g.drawDashedLine ({ x1, r.getY() + 18.0f, x1, r.getBottom() }, dash, 2, 1.0f);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.setColour (kText);
            g.drawText ("RT60 " + juce::String (rt60, rt60 < 10.0f ? 2 : 1) + " s", juce::Rectangle<float> (x1 - 130.0f, r.getY() + 4.0f, 124.0f, 14.0f), juce::Justification::centredRight, false);
            g.drawText ("pre " + juce::String (juce::roundToInt (preMs)) + " ms", juce::Rectangle<float> (x0 + 8.0f, decayYOfDb (r, 0.0f) - 7.0f, 120.0f, 14.0f), juce::Justification::centredLeft, false);
            node (g, x0, decayYOfDb (r, 0.0f), s.accent);
            node (g, juce::jmin (x1, r.getRight() - 4.0f), r.getBottom() - 4.0f, s.accent);
        }
    }

    void drawConv (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        drawDecay (g, r, s, s.fx != nullptr && s.m (0) > 0.0f ? s.m (0) : s.P (0), s.P (1));
        if (s.detailed)
        {
            const int ir = juce::jlimit (0, 10, s.I (5));
            const juce::String name = ir == 10 ? (s.assetName.isNotEmpty() ? "User: " + s.assetName : juce::String ("User file - none loaded (Hall)"))
                                               : juce::String (s.d->p[5].choices[ir]);
            label (g, r, name, juce::Justification::topLeft, 11.0f, kText);
        }
    }

    void drawTaps (juce::Graphics& g, juce::Rectangle<float> r, const State& s, const float* times, const float* gains, const bool* rightSide, int n)
    {
        const float span = delaySpanMs (s);
        timeGrid (g, r, s, span);
        g.setColour (kText); g.fillRect (r.getX(), r.getY() + r.getHeight() * 0.15f, 2.0f, r.getHeight() * 0.85f);
        for (int k = 0; k < n; ++k)
        {
            if (times[k] > span) continue;
            const float x = xOfMs (r, times[k], span);
            const float h = clamp01 (gains[k]) * r.getHeight() * 0.85f;
            g.setColour (rightSide[k] ? s.accent.withAlpha (0.5f) : s.accent);
            g.fillRect (x - 1.5f, r.getBottom() - h, 3.0f, h);
        }
        if (s.detailed && n > 0)
        {
            const auto p0 = delayTapPos (r, s, 0), p1 = delayTapPos (r, s, 1);
            node (g, p0.x, p0.y, s.accent);
            node (g, p1.x, p1.y, s.accent, false);
            g.setColour (s.accent.withAlpha (0.5f));
            const float dash[] = { 3.0f, 3.0f };
            g.drawDashedLine ({ p0.x, p0.y, p1.x + 0.01f, p1.y }, dash, 2, 1.0f);
            juce::String t = delaySynced (s) ? juce::String (s.d->p[2].choices[juce::jlimit (0, 14, s.I (2))]) + "  " + juce::String (juce::roundToInt (delayBaseMs (s))) + " ms"
                                             : juce::String (delayBaseMs (s), delayBaseMs (s) < 10.0f ? 1 : 0) + " ms";
            g.setColour (kText);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (t, juce::Rectangle<float> (p0.x + 8.0f, p0.y - 6.0f, 140.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }

    void drawDelay (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        float times[kMaxTaps], gains[kMaxTaps];
        bool right[kMaxTaps];
        const int n = delayTaps (s, times, gains, right);
        drawTaps (g, r, s, times, gains, right, n);
    }

    void drawSpecDelay (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 8, 4);
        const int nb = s.I (6) == 0 ? 4 : (s.I (6) == 1 ? 6 : 8);
        const float maxMs = 4000.0f;
        for (int b = 0; b < nb; ++b)
        {
            const float y = r.getY() + r.getHeight() * ((float) (nb - 1 - b) + 0.5f) / (float) nb;
            const float ms = s.P (0) * juce::jlimit (0.05f, 8.0f, s.data[b * 4]) * std::pow (2.0f, s.P (1) * 2.0f * ((float) b / (float) juce::jmax (1, nb - 1) - 0.5f));
            const float x = r.getX() + clamp01 (std::log (1.0f + ms) / std::log (1.0f + maxMs)) * r.getWidth();
            const float fb = clamp01 (s.data[b * 4 + 1] * s.P (2));
            g.setColour (s.accent.withAlpha (0.25f + 0.5f * fb));
            g.fillRect (r.getX(), y - 1.0f - 3.0f * fb, x - r.getX(), 2.0f + 6.0f * fb);
            node (g, x, y, s.accent, true, s.detailed ? 5.0f : 3.5f);
        }
        if (s.detailed) label (g, r, "drag a band: x = time, y = feedback", juce::Justification::topRight, 9.0f, kDim);
    }

    void drawGrains (juce::Graphics& g, juce::Rectangle<float> r, const State& s, int count)
    {
        grid (g, r, 8, 4);
        ghostWave (g, s, r, 0.2f);
        for (int k = 0; k < count; ++k)
        {
            const float age = s.m (k);
            if (age < 0.0f) continue;
            const float x = r.getX() + r.getWidth() * (0.05f + 0.9f * (float) ((k * 37) % count) / (float) count);
            const float y = r.getY() + r.getHeight() * (0.15f + 0.7f * (float) ((k * 53) % 17) / 17.0f);
            const float sz = 3.0f + 7.0f * std::sin (kPi * age);
            g.setColour (s.accent.withAlpha (0.35f + 0.5f * std::sin (kPi * age)));
            g.fillEllipse (x - sz * 0.5f, y - sz * 0.5f, sz, sz);
        }
    }

    void drawEarly (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        grid (g, r, 8, 4);
        const int taps = juce::jlimit (4, 16, s.I (3));
        const float scale = 0.3f + 1.4f * s.P (0);
        const float base = s.mode == 0 ? 2.5f : (s.mode == 1 ? 7.0f : 0.0f);
        const float span = 140.0f;
        for (int k = 0; k < taps; ++k)
        {
            const float ms = s.P (1) + (s.mode == 2 ? 55.0f + 45.0f * (float) (k % 4) : base * std::pow ((float) (k + 1), 0.95f)) * scale;
            const float a = s.mode == 2 ? 0.8f * std::pow (0.6f, (float) (k % 4)) : 0.9f * std::exp (-0.13f * (float) k);
            const float x = r.getX() + juce::jmin (1.0f, ms / span) * r.getWidth();
            g.setColour ((k & 1) ? s.accent.withAlpha (0.6f) : s.accent);
            g.fillRect (x - 1.5f, r.getBottom() - a * r.getHeight() * 0.85f, 3.0f, a * r.getHeight() * 0.85f);
        }
    }

    // spectral comp: per-band threshold offsets (editable) over the gain change
    void drawSpecComp (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        const int nb = s.I (4) == 0 ? 4 : (s.I (4) == 1 ? 8 : 16);
        grid (g, r, juce::jmin (nb, 8), 4);
        g.setColour (kAxis); g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
        const float bw = r.getWidth() / (float) nb;
        for (int b = 0; b < nb; ++b)
        {
            auto col = juce::Rectangle<float> (r.getX() + bw * (float) b, r.getY(), bw, r.getHeight()).reduced (juce::jmin (5.0f, bw * 0.15f), 2.0f);
            const float gdb = s.m (b);
            const float h = juce::jlimit (-1.0f, 1.0f, gdb / 24.0f) * col.getHeight() * 0.5f;
            g.setColour ((gdb < 0.0f ? kRed : s.accent).withAlpha (0.7f));
            g.fillRect (col.getX(), h > 0 ? col.getCentreY() - h : col.getCentreY(), col.getWidth(), std::abs (h));
            const float off = juce::jlimit (-24.0f, 24.0f, s.data[b]);
            const float y = col.getCentreY() - off / 24.0f * col.getHeight() * 0.5f;
            g.setColour (kAmber);
            g.fillRect (col.getX(), y - 1.0f, col.getWidth(), 2.0f);
        }
        if (s.detailed) label (g, r, "threshold offsets (drag)", juce::Justification::topRight, 9.0f, kAmber);
    }

    void drawResSupp (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        const int nb = s.I (7) == 0 ? 16 : (s.I (7) == 1 ? 24 : 32);
        drawBands (g, r, s, nb, 24.0f, false);
        const float lo = 60.0f, hi = 14000.0f;
        auto xOf = [&] (float f) { return r.getX() + r.getWidth() * std::log (juce::jlimit (lo, hi, f) / lo) / std::log (hi / lo); };
        g.setColour (s.accent.withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (s.P (2)), r.getY(), xOf (s.P (3)), r.getBottom()));
        if (s.detailed) { node (g, xOf (s.P (2)), r.getBottom() - 12.0f, s.accent); node (g, xOf (s.P (3)), r.getBottom() - 12.0f, s.accent); }
    }

    // ================================================================ time
    void drawEuclid (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        const int steps = juce::jlimit (2, 32, s.I (1)), pulses = juce::jlimit (0, steps, s.I (2)), rot = s.I (3);
        const auto c = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.4f;
        const int cur = (int) s.m (0);
        for (int k = 0; k < steps; ++k)
        {
            const float a = kTwoPi * (float) k / (float) steps - kPi * 0.5f;
            const int ss = ((k + rot) % steps + steps) % steps;
            const bool hit = ((ss * pulses) % steps) < pulses;
            const float d = (steps > 20 ? 5.0f : 8.0f) * (s.detailed ? 1.3f : 1.0f);
            const float x = c.x + std::cos (a) * rad, y = c.y + std::sin (a) * rad;
            g.setColour (hit ? s.accent.withAlpha (k == cur ? 1.0f : 0.7f) : kAxis);
            g.fillEllipse (x - d * 0.5f, y - d * 0.5f, d, d);
            if (k == cur) { g.setColour (juce::Colours::white); g.drawEllipse (x - d * 0.5f - 2.0f, y - d * 0.5f - 2.0f, d + 4.0f, d + 4.0f, 1.2f); }
        }
        g.setColour (s.accent.withAlpha (0.25f + 0.6f * clamp01 (s.m (2))));
        g.fillEllipse (c.x - rad * 0.35f, c.y - rad * 0.35f, rad * 0.7f, rad * 0.7f);
    }

    void drawProgress (juce::Graphics& g, juce::Rectangle<float> r, const State& s, float pos01, const juce::String& text, bool reverse = false)
    {
        grid (g, r, 4, 4);
        ghostWave (g, s, r, 0.55f);
        const float x = r.getX() + clamp01 (reverse ? 1.0f - pos01 : pos01) * r.getWidth();
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillRect (x - 0.5f, r.getY(), 1.0f, r.getHeight());
        if (text.isNotEmpty()) label (g, r, text, juce::Justification::topLeft, s.detailed ? 12.0f : 10.0f, s.accent);
    }

    void drawTapeStop (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        const float speed = s.fx != nullptr ? clamp01 (s.m (0)) : s.P (1);
        const auto c = juce::Point<float> (r.getCentreX(), r.getBottom() - 6.0f);
        const float rad = juce::jmin (r.getWidth() * 0.45f, r.getHeight() - 12.0f);
        juce::Path arc; arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, -kPi * 0.5f, kPi * 0.5f, true);
        g.setColour (kAxis); g.strokePath (arc, juce::PathStrokeType (4.0f));
        juce::Path val; val.addCentredArc (c.x, c.y, rad, rad, 0.0f, -kPi * 0.5f, -kPi * 0.5f + kPi * speed, true);
        g.setColour (s.accent); g.strokePath (val, juce::PathStrokeType (4.0f));
        const float a = -kPi * 0.5f + kPi * speed;
        g.setColour (juce::Colours::white);
        g.drawLine (c.x, c.y, c.x + std::sin (a) * rad * 0.85f, c.y - std::cos (a) * rad * 0.85f, 2.0f);
    }
}

// ====================================================================
juce::Rectangle<float> plotArea (juce::Rectangle<float> full, bool detailed) { return full.reduced (detailed ? 6.0f : 3.0f); }

void responseRange (Viz v, float& lo, float& hi)
{
    switch (v)
    {
        case VizEq3: case VizEq8: lo = -18.0f; hi = 18.0f; return;
        case VizDeess: lo = -36.0f; hi = 6.0f; return;
        case VizHum: lo = -48.0f; hi = 6.0f; return;
        case VizWavetable: lo = -48.0f; hi = 18.0f; return;
        default: lo = -36.0f; hi = 24.0f; return;
    }
}


bool curveBipolar (Viz v) { return isLfoViz (v); }

float curveScale (const State& s)
{
    if (s.d == nullptr) return 1.0f;
    if (s.d->viz == VizDuck) return juce::jmax (0.1f, s.P (0));
    if (isLfoViz (s.d->viz)) return lfoDepth (s);
    return 1.0f;
}

bool isLfoViz (Viz v)
{
    return v == VizAutoPan || v == VizChorus || v == VizFlanger || v == VizPhaser || v == VizTremolo || v == VizVibrato || v == VizWobble || v == VizWah;
}

int lfoShapeIndex (const EffectDef& d)
{
    for (int k = 0; k < d.numParams; ++k)
        if (juce::String (d.p[k].name) == "Shape" && d.p[k].numChoices == LfoShapes::kNum) return k;
    return -1;
}

int lfoMorphIndex (const EffectDef& d)
{
    for (int k = 0; k < d.numParams; ++k)
        if (juce::String (d.p[k].name) == "Morph") return k;
    return -1;
}

const char* chordQualityName (int q)
{
    return kChordQualities[juce::jlimit (0, kNumChordQualities - 1, q)].name;
}

juce::String noteName (int midi, bool flats)
{
    static const char* const sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* const flatN[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    const int k = ((midi % 12) + 12) % 12;
    return flats ? flatN[k] : sharps[k];
}

void drawEffectVisual (juce::Graphics& g, juce::Rectangle<float> full, const State& s)
{
    juce::Graphics::ScopedSaveState save (g);
    juce::Path clip; clip.addRoundedRectangle (full, 6.0f);
    g.reduceClipRegion (clip);
    g.setColour (juce::Colour (0xFF1A1C21));
    g.fillRoundedRectangle (full, 6.0f);
    if (s.d == nullptr) return;
    const auto r = plotArea (full, s.detailed);
    float lo, hi;
    responseRange (s.d->viz, lo, hi);

    switch (s.d->viz)
    {
        // analysis
        case VizSpectrum:    drawSpectrum (g, r, s); break;
        case VizScope:       drawScope (g, r, s); break;
        case VizLoudness:    drawLoudness (g, r, s); break;
        case VizStereoScope: drawGonio (g, r.withTrimmedBottom (s.detailed ? 18.0f : 8.0f), s, s.mode == 1, s.P (0), s.I (2) == 1, s.P (1));
                             corrBar (g, r, s, s.fx != nullptr ? s.fx->taps().corr.load() : 0.0f); break;
        case VizPitchDet:    drawPitchDet (g, r, s); break;
        case VizChordDet:    drawChord (g, r, s); break;
        // dynamics
        case VizComp: case VizLimit: case VizSidechain: case VizGate: case VizExpander: case VizSquash:
            drawTransferDb (g, r, s); break;
        case VizOtt:     drawOtt (g, r, s); break;
        case VizDenoise: drawDenoise (g, r, s); break;
        // EQ
        case VizEq3:   drawEq3 (g, r, s); break;
        case VizEq8:   drawEq8 (g, r, s); break;
        case VizDeess: drawResponse (g, r, s, lo, hi, 0.0f, true); drawFilterNode (g, r, s, lo, hi, 0); break;
        case VizHum:   drawResponse (g, r, s, lo, hi); break;
        case VizClick: drawProgress (g, r, s, 0.0f, juce::String (juce::roundToInt (s.m (0))) + " clicks/s"); break;
        // volume
        case VizHarmRes:   drawHarmRes (g, r, s); break;
        case VizMbGate:    drawMbGate (g, r, s); break;
        case VizTransient: drawTransient (g, r, s); break;
        case VizDuck:      drawDucker (g, r, s); break;
        case VizTrance:
        {
            const int steps = juce::jlimit (1, Shape::kSteps, s.I (1));
            const float depth = s.P (2);
            drawSteps (g, r, s, steps, s.fx != nullptr ? (int) s.m (1) : -1, [&] (int k) { return 1.0f - depth * (1.0f - clamp01 (s.data[k])); });
            break;
        }
        case VizGain: drawGain (g, r, s); break;
        // filters
        case VizMultimode: case VizSvf: case VizLadder: case VizVintage:
            drawResponse (g, r, s, lo, hi, s.m (0), true); drawFilterNode (g, r, s, lo, hi, 0); break;
        case VizComb:    drawResponse (g, r, s, lo, hi, 0.0f, true); drawFilterNode (g, r, s, lo, hi, 0); break;
        case VizFormant: drawResponse (g, r, s, lo, hi, 0.0f, true); break;
        case VizWavetable: drawWavetableFilter (g, r, s); break;
        case VizAllpass:   drawAllpass (g, r, s); break;
        // modulation
        case VizAutoPan: case VizTremolo: case VizChorus: case VizFlanger: case VizPhaser: case VizVibrato: case VizWobble:
            drawLfo (g, r, s); break;
        case VizWah:
            if (s.mode == 1) drawLfo (g, r, s); else drawWah (g, r, s);
            break;
        case VizRingMod: case VizFm:
        {
            const int shapeIdx = s.d->viz == VizRingMod ? 2 : 4;
            bigText (g, r, s, juce::String (juce::roundToInt (s.fx != nullptr && s.m (1) > 0.0f ? s.m (1) : s.P (0))) + " Hz", s.d->p[shapeIdx].choices[juce::jlimit (0, 2, s.I (shapeIdx))]);
            break;
        }
        case VizDoppler: drawDoppler (g, r, s); break;
        case VizFShift:  bigText (g, r, s, (s.P (0) >= 0 ? "+" : "") + juce::String (juce::roundToInt (s.P (0))) + " st", s.m (0) > 0.0f ? noteStr (freqToMidi (s.m (0))) : juce::String ("--")); break;
        // stereo
        case VizWidth:    drawStereoImage (g, r, s, s.P (0)); break;
        case VizHaas:     drawHaas (g, r, s); break;
        case VizMonoBass: drawMonoBass (g, r, s); break;
        // pitch
        case VizFreqShift:  drawShiftArrow (g, r, s, 440.0f, juce::jmax (21.0f, 440.0f + s.P (0))); break;
        case VizPitchShift: bigText (g, r, s, (s.P (0) >= 0 ? "+" : "") + juce::String (juce::roundToInt (s.P (0))) + " st",
                                     (s.P (1) >= 0 ? "+" : "") + juce::String (juce::roundToInt (s.P (1))) + " ct"); break;
        case VizPitchCorr:
            if (s.m (0) > 0.0f) bigText (g, r, s, noteStr (s.m (1)), (s.m (2) >= 0 ? "+" : "") + juce::String (juce::roundToInt (s.m (2) * 100.0f)) + " ct");
            else bigText (g, r, s, "--");
            break;
        case VizHarmonizer:
        {
            juce::String txt = juce::String (s.d->p[2].choices[juce::jlimit (0, 8, s.I (2))]);
            if (s.I (3) > 0) txt << "  " << s.d->p[3].choices[juce::jlimit (0, 9, s.I (3))];
            bigText (g, r, s, s.m (0) > 0.0f ? noteStr (s.m (0)) : juce::String ("--"), txt); break;
        }
        case VizVocoder:   drawVocoder (g, r, s); break;
        case VizResonator: drawResonator (g, r, s); break;
        // drive
        case VizDrive: case VizSat: case VizFuzz: case VizWaveFolder: case VizWtDrive:
        case VizClipper: case VizBitcrush:
            drawTransferLin (g, r, s); break;
        case VizWaveShaper: drawWaveshaper (g, r, s); break;
        case VizAdditive:
            if (s.detailed) drawHarmonicBars (g, r, s); else drawTransferLin (g, r, s);
            break;
        case VizMbDrive: drawMbDrive (g, r, s); break;
        case VizPDist: case VizLofi:
            grid (g, r, 8, 4); ghostWave (g, s, r, 0.85f); break;
        // space
        case VizEarly: drawEarly (g, r, s); break;
        case VizRoom:    drawDecay (g, r, s, s.P (1), s.P (2)); break;
        case VizPlate:   drawDecay (g, r, s, s.P (0), s.P (1)); break;
        case VizSpring:  drawDecay (g, r, s, s.P (0), 0.0f); break;
        case VizShimmer: drawDecay (g, r, s, s.P (2), s.P (3)); break;
        case VizAlgo:    drawDecay (g, r, s, s.fx != nullptr && s.m (0) > 0.0f ? s.m (0) : s.P (1), s.P (2)); break;
        case VizConv:    drawConv (g, r, s); break;
        case VizSimpleDly: case VizPrecDly: case VizEcho: case VizWarp: case VizPingPong:
            drawDelay (g, r, s); break;
        case VizSpecDly: drawSpecDelay (g, r, s); break;
        case VizGranDly: drawGrains (g, r, s, 24); break;
        // spectral
        case VizSpecComp: drawSpecComp (g, r, s); break;
        case VizResSupp:  drawResSupp (g, r, s); break;
        case VizExciter:  drawShiftArrow (g, r, s, s.mode == 1 ? s.P (1) / 20.0f : s.P (1), s.mode == 1 ? s.P (1) / 10.0f : juce::jmin (19000.0f, s.P (1) * 2.0f)); break;
        // time
        case VizGranular:    drawGrains (g, r, s, 32); break;
        case VizTimeStretch: drawProgress (g, r, s, s.m (0), juce::String (s.d->p[0].choices[juce::jlimit (0, 2, s.I (0))]) + " speed"); break;
        case VizReverse:     drawProgress (g, r, s, s.m (0), "", true); break;
        case VizTapeStop:    drawTapeStop (g, r, s); break;
        case VizStutter:     drawSteps (g, r, s, juce::jlimit (1, 16, s.I (2)), (int) s.m (1) - 1, [&] (int k) { return s.P (3) * dbToGain (s.P (7) * (float) k); }); break;
        case VizShuffler:    drawProgress (g, r, s, s.m (0), s.m (1) > 1.5f ? "REV" : (s.m (1) > 0.5f ? "SHUFFLE" : "")); break;
        case VizSlicer:      drawEuclid (g, r, s); break;
        case VizGlitch:
        {
            static const char* const kinds[6] = { "", "REPEAT", "REVERSE", "CRUSH", "PITCH", "STOP" };
            drawProgress (g, r, s, s.m (0), kinds[juce::jlimit (0, 5, (int) s.m (1))]); break;
        }
        default: grid (g, r, 8, 4); ghostWave (g, s, r, 0.8f); break;
    }
}

juce::String readoutTag (const State& s)
{
    if (s.d == nullptr) return "-";
    if (s.fx == nullptr) return s.d->name;
    const auto& t = s.fx->taps();
    auto m = [&] (int i) { return t.get (i); };
    auto hz = [] (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz"; };
    switch (s.d->viz)
    {
        case VizSpectrum:   return "FFT 2048";
        case VizScope:      return juce::String (s.P (0), 1) + " ms";
        case VizLoudness:   { const float i = t.lufsI.load(); return i > -99.0f ? juce::String (i, 1) + " LUFS" : juce::String ("-- LUFS"); }
        case VizStereoScope:return "Corr " + juce::String (t.corr.load(), 2);
        case VizPitchDet:   { const float f = t.pitchHz.load(); return f > 0.0f ? juce::String (f, 1) + " Hz" : juce::String ("--"); }
        case VizChordDet:   return m (0) >= 0.0f ? juce::String (juce::roundToInt (m (2) * 100.0f)) + "% match" : juce::String ("no chord");
        case VizComp: case VizLimit: case VizSidechain: case VizSquash: case VizDeess:
            return "GR " + juce::String (juce::jmin (0.0f, m (0)), 1) + " dB";
        case VizGate: case VizExpander: return juce::String (m (0), 1) + " dB";
        case VizClick:     return juce::String (juce::roundToInt (m (0))) + " clicks/s";
        case VizTransient: return (m (0) >= 0 ? "+" : "") + juce::String (m (0), 1) + " dB";
        case VizMbGate:
        {
            int closed = 0;
            for (int b = 0; b < Shape::kMbgBands; ++b) if (s.data[b * Shape::kMbgStride + 2] > 0.5f && m (Shape::kMbgBands + b) < -1.0f) ++closed;
            return juce::String (closed) + " / 5 closed";
        }
        case VizHarmRes:   return harmres::setName (s.p, s.mode);
        case VizTrance:    return "Step " + juce::String ((int) m (1) + 1);
        case VizMultimode: case VizSvf: case VizLadder: case VizVintage: return hz (m (0));
        case VizWavetable: return (s.table != nullptr ? s.table->name : juce::String ("Built-in")) + " " + juce::String (juce::roundToInt (m (0) * 100.0f)) + "%";
        case VizWtDrive:   return (s.table != nullptr ? s.table->name : juce::String ("Built-in")) + " " + juce::String (juce::roundToInt (m (1) * 100.0f)) + "%";
        case VizPitchCorr: return m (0) > 0.0f ? noteStr (m (0)) + " > " + noteStr (m (1)) : juce::String ("--");
        case VizRoom: case VizPlate: case VizSpring: case VizShimmer: case VizAlgo: case VizConv:
            return "RT60 " + juce::String (m (0), 1) + " s";
        case VizSimpleDly: case VizPrecDly: case VizPingPong: case VizEcho: case VizWarp:
            return juce::String (m (0), m (0) < 100.0f ? 1 : 0) + " ms";
        case VizClipper:   return "Over " + juce::String (m (1), 1) + " dB";
        case VizTapeStop:  return juce::String (juce::roundToInt (m (0) * 100.0f)) + "% speed";
        case VizSlicer:    return "Step " + juce::String ((int) m (0) + 1) + " / " + juce::String (s.I (1));
        case VizStutter:   return m (1) > 0.0f ? "Repeat " + juce::String ((int) m (1)) : juce::String ("Live");
        case VizDoppler:   return juce::String (m (2), 1) + " m away";
        case VizAllpass:   return juce::String (s.I (2)) + " stages";
        case VizEq3: case VizEq8: case VizHum: return "Out " + (s.fx->peakDb() > -99.0f ? juce::String (s.fx->peakDb(), 1) + " dBFS" : juce::String ("--"));
        case VizWidth: case VizHaas: case VizMonoBass: return "Corr " + juce::String (m (0), 2);
        case VizOtt:       return "L " + juce::String (m (0), 1) + "  M " + juce::String (m (1), 1) + "  H " + juce::String (m (2), 1);
        default: break;
    }
    return s.fx->peakDb() > -99.0f ? "Peak " + juce::String (s.fx->peakDb(), 1) + " dBFS" : juce::String ("Silent");
}

} // namespace viz

int viz::delayTaps (const State& s, float* times, float* gains, bool* right)
{
    constexpr int kMax = kMaxTaps;
    int n = 0;
    float msR = 0.0f;
    const float msL = delayBaseMs (s, &msR);
    if (s.d->viz == VizPrecDly && s.mode == 0)
    {
        const int reps = juce::jlimit (1, 16, s.I (8));
        const float rr = std::pow (2.0f, s.P (10) * 0.6f);
        float acc = 0.0f, step = msL;
        const bool ping = s.I (11) == 1;
        for (int k = 0; k < reps; ++k)
        {
            acc += step; step *= rr;
            times[n] = acc; gains[n] = dbToGain (s.P (9) * (float) k); right[n] = ping && (k & 1); ++n;
        }
    }
    else
    {
        const float fb = s.d->viz == VizWarp ? s.P (2) : s.P (3);
        const bool pp = s.d->viz == VizPingPong || (s.d->viz == VizPrecDly && s.I (11) == 1);
        float a = 1.0f;
        for (int k = 1; k <= kMax && a > 0.02f && n < kMax - 1; ++k)
        {
            times[n] = msL * (float) k; gains[n] = a; right[n] = pp && (k % 2 == 0); ++n;
            if (! pp && std::abs (msR - msL) > 0.5f && n < kMax) { times[n] = msR * (float) k; gains[n] = a; right[n] = true; ++n; }
            a *= juce::jmin (1.0f, fb);
        }
    }
    return n;
}

float viz::divisionBeats (int i)
{
    // "1/1", "1/2", "1/2D", "1/2T", "1/4", "1/4D", "1/4T", "1/8", "1/8D", "1/8T", "1/16", "1/16D", "1/16T", "1/32", "1/64"
    static const float beats[] = { 4.0f, 2.0f, 3.0f, 4.0f / 3.0f, 1.0f, 1.5f, 2.0f / 3.0f, 0.5f, 0.75f, 1.0f / 3.0f, 0.25f, 0.375f, 1.0f / 6.0f, 0.125f, 0.0625f };
    return beats[juce::jlimit (0, 14, i)];
}

float viz::delayBpm (const State& s)
{
    // the effect reports its running delay time; with sync on that gives the host tempo
    if (delaySynced (s) && s.fx != nullptr && s.m (0) > 1.0f)
        return juce::jlimit (20.0f, 999.0f, divisionBeats (s.I (2)) * 60000.0f / s.m (0));
    return 120.0f;
}

float viz::delayBaseMs (const State& s, float* msR)
{
    const float ms = delaySynced (s) ? divisionBeats (s.I (2)) * 60000.0f / delayBpm (s) : s.P (1);
    if (msR != nullptr)
    {
        *msR = ms;
        if (s.d->viz == efx::VizPrecDly) *msR = juce::jmax (0.1f, ms + s.P (4));   // R offset
    }
    return ms;
}

float viz::niceSpanMs (float need)
{
    for (float v : { 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f, 32000.0f, 64000.0f })
        if (v >= need) return v;
    return 64000.0f;
}

float viz::delaySpanMs (const State& s)
{
    if (s.spanMs > 0.0f) return s.spanMs;
    float times[kMaxTaps], gains[kMaxTaps];
    bool right[kMaxTaps];
    const int n = delayTaps (s, times, gains, right);
    // room for the first few repeats, at least four
    float need = 1.0f;
    for (int k = 0; k < juce::jmin (n, 4); ++k) need = juce::jmax (need, times[k]);
    return niceSpanMs (juce::jmax (need, delayBaseMs (s) * 4.0f) * 1.1f);
}

float viz::decaySpanMs (const State& s)
{
    if (s.spanMs > 0.0f) return s.spanMs;
    int iDecay = 0, iPre = -1;
    const float rt = decayShownRt (s, iDecay, iPre), pre = iPre >= 0 ? s.P (iPre) : 0.0f;
    return niceSpanMs ((pre + rt * 1000.0f) * 1.25f);
}

juce::Point<float> viz::delayTapPos (juce::Rectangle<float> r, const State& s, int k)
{
    float times[kMaxTaps], gains[kMaxTaps];
    bool right[kMaxTaps];
    const int n = delayTaps (s, times, gains, right);
    if (n == 0) return r.getBottomLeft();
    // the second "repeat" handle is the next tap on the same base time
    int idx = 0;
    if (k > 0)
    {
        idx = -1;
        for (int i = 1; i < n; ++i) if (times[i] > times[0] * 1.5f) { idx = i; break; }
        if (idx < 0) return { xOfMs (r, times[0] * 2.0f, delaySpanMs (s)), r.getBottom() - 4.0f };
    }
    return { xOfMs (r, times[idx], delaySpanMs (s)), r.getBottom() - juce::jlimit (0.0f, 1.0f, gains[idx]) * r.getHeight() * 0.85f };
}

float viz::transientSpanMs (const State& s)
{
    if (s.spanMs > 0.0f) return s.spanMs;
    return niceSpanMs ((s.P (6) * 2.0f + s.P (5) * 1.4f) * 1.1f);
}

float viz::transientCurveDb (const State& s, float ms)
{
    const float dec = juce::jmax (1.0f, s.P (6)), rel = juce::jmax (1.0f, s.P (5));
    const float a = s.P (0) * 12.0f, su = s.P (1) * 12.0f;
    const float fadeIn = 1.0f - std::exp (-3.0f * ms / dec);
    const float tail = ms < dec + rel ? 1.0f : std::exp (-(ms - dec - rel) / (0.15f * rel));
    return a * std::exp (-3.0f * ms / dec) + su * fadeIn * tail;
}

juce::Range<float> viz::mbgBandX (juce::Rectangle<float> r, const State& s, int b)
{
    const float lo = b == 0 ? r.getX() : xOfHz (r, Shape::mbgCrossover (s.p, b - 1));
    const float hi = b == Shape::kMbgBands - 1 ? r.getRight() : xOfHz (r, Shape::mbgCrossover (s.p, b));
    return { lo, hi };
}

juce::Rectangle<float> viz::harmResKeys (juce::Rectangle<float> r, bool detailed)
{
    const float h = juce::jlimit (14.0f, detailed ? 70.0f : 30.0f, r.getHeight() * 0.26f);
    return r.withTop (r.getBottom() - h);
}

juce::Rectangle<float> viz::harmResCurve (juce::Rectangle<float> r, bool detailed)
{
    return r.withBottom (harmResKeys (r, detailed).getY() - 4.0f);
}

juce::Rectangle<float> viz::harmResKeyRect (juce::Rectangle<float> keys, int pc)
{
    static const int whiteIdx[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
    static const float blackAt[12] = { 0, 1, 0, 2, 0, 0, 4, 0, 5, 0, 6, 0 };   // after white key n
    const float ww = keys.getWidth() / 7.0f;
    if (whiteIdx[pc] >= 0) return { keys.getX() + ww * (float) whiteIdx[pc], keys.getY(), ww, keys.getHeight() };
    const float bw = ww * 0.6f;
    return { keys.getX() + ww * blackAt[pc] - bw * 0.5f, keys.getY(), bw, keys.getHeight() * 0.6f };
}

int viz::harmResKeyAt (juce::Rectangle<float> keys, juce::Point<float> p)
{
    if (! keys.contains (p)) return -1;
    for (int pc : { 1, 3, 6, 8, 10 }) if (harmResKeyRect (keys, pc).contains (p)) return pc;
    for (int pc : { 0, 2, 4, 5, 7, 9, 11 }) if (harmResKeyRect (keys, pc).contains (p)) return pc;
    return -1;
}

float viz::decayShownRt (const State& s, int& iDecay, int& iPre)
{
    iDecay = 0; iPre = -1;
    switch (s.d->viz)
    {
        case efx::VizRoom: iDecay = 1; iPre = 2; break;
        case efx::VizPlate: iDecay = 0; iPre = 1; break;
        case efx::VizShimmer: iDecay = 2; iPre = 3; break;
        case efx::VizAlgo: iDecay = 1; iPre = 2; break;
        case efx::VizConv: iDecay = 0; iPre = 1; break;
        default: break;
    }
    if ((s.d->viz == efx::VizAlgo || s.d->viz == efx::VizConv) && s.fx != nullptr && s.m (0) > 0.0f) return s.m (0);
    return s.P (iDecay);
}
