#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>
#include <cmath>

// ============================================================
// Effect sequencer: per-slot on/off lanes clocked by the host.
// Data, rate table and a pure scheduler (no GUI, no allocation) that turns
// a lane and the host position into gate events for one audio block.
//
//   - A lane is a 32-bit step mask per scene, plus optional own length and
//     rate (polyrhythms). Lane mode (Always / Seq) and tail (Cut / Ring)
//     belong to the slot and move with it.
//   - Step position comes straight from the host's PPQ, never counted, so
//     lanes can't drift and line up again at the song start.
//   - Neighbouring on steps are one run: one gate on, one gate off; a run
//     that wraps past the last step into step 1 stays open.
// ============================================================
namespace seq
{
    inline constexpr int kScenes = 16, kMaxSteps = 32, kMaxEvents = 32;
    enum LaneMode : int { Always = 0, Sequenced = 1 };
    enum Tail : int { Cut = 0, Ring = 1 };

    // step lengths: 1/1 .. 1/64, each straight, dotted, triplet
    inline constexpr int kNumRates = 21;
    inline constexpr int kRate16th = 12;   // default 1/16
    inline const char* rateName (int i)
    {
        static const char* const names[kNumRates] = {
            "1/1", "1/1D", "1/1T", "1/2", "1/2D", "1/2T", "1/4", "1/4D", "1/4T", "1/8", "1/8D", "1/8T",
            "1/16", "1/16D", "1/16T", "1/32", "1/32D", "1/32T", "1/64", "1/64D", "1/64T" };
        return names[juce::jlimit (0, kNumRates - 1, i)];
    }
    // beats (quarter notes) per step
    inline double rateBeats (int i)
    {
        i = juce::jlimit (0, kNumRates - 1, i);
        const double base = 4.0 / (double) (1 << (i / 3));   // 1/1 = 4 beats
        const int kind = i % 3;
        return kind == 0 ? base : (kind == 1 ? base * 1.5 : base * 2.0 / 3.0);
    }

    // one lane of one scene; len / rate 0 / -1 = use the scene's
    struct Lane
    {
        uint32_t steps = 0;
        int len = 0, rate = -1;
        bool operator== (const Lane& o) const { return steps == o.steps && len == o.len && rate == o.rate; }
        bool operator!= (const Lane& o) const { return ! (*this == o); }
    };

    struct SceneSettings
    {
        int steps = 16, rate = kRate16th;
        float swing = 0.0f;   // 0 .. 0.75: every second step is delayed by swing x step
        bool operator== (const SceneSettings& o) const { return steps == o.steps && rate == o.rate && std::abs (swing - o.swing) < 1.0e-6f; }
    };

    // a slot's sequencer data (moves with the slot)
    struct SlotLanes
    {
        Lane lanes[kScenes];
        int mode = Always, tail = Cut;
        bool operator== (const SlotLanes& o) const
        {
            if (mode != o.mode || tail != o.tail) return false;
            for (int i = 0; i < kScenes; ++i) if (lanes[i] != o.lanes[i]) return false;
            return true;
        }
    };

    // the timing of one lane as the scheduler needs it
    struct LaneClock
    {
        uint32_t steps = 0;
        int len = 16;
        double stepBeats = 0.25;
        double swing = 0.0;

        static LaneClock make (const Lane& l, const SceneSettings& sc)
        {
            LaneClock c;
            c.steps = l.steps;
            c.len = juce::jlimit (1, kMaxSteps, l.len > 0 ? l.len : sc.steps);
            c.stepBeats = rateBeats (l.rate >= 0 ? l.rate : sc.rate);
            c.swing = juce::jlimit (0.0, 0.75, (double) sc.swing);
            return c;
        }
        bool bit (long long absStep) const
        {
            const int i = (int) (((absStep % len) + len) % len);
            return ((steps >> i) & 1u) != 0;
        }
        // steps come in pairs; the second of each pair starts swing x step late
        long long stepAt (double ppq) const
        {
            const double pair = 2.0 * stepBeats, firstLen = stepBeats * (1.0 + swing);
            const double pairs = std::floor (ppq / pair);
            const double inPair = ppq - pairs * pair;
            return (long long) pairs * 2 + (inPair < firstLen ? 0 : 1);
        }
        double stepStart (long long absStep) const
        {
            const double pair = 2.0 * stepBeats;
            const long long p = absStep >= 0 ? absStep / 2 : -((-absStep + 1) / 2);
            return (double) p * pair + ((absStep - p * 2) != 0 ? stepBeats * (1.0 + swing) : 0.0);
        }
        bool openAt (double ppq) const { return bit (stepAt (ppq)); }
    };

    // a gate change at a sample offset; for a gate-on also the run it starts:
    // its full length and how far into it we already are (beats)
    struct GateEvent
    {
        int offset = 0;
        bool on = false;
        double runBeats = 0.0, phaseBeats = 0.0;
    };

    // the run of on steps that contains absStep (which must be on)
    inline void runAround (const LaneClock& c, long long absStep, double& startBeat, double& lengthBeats)
    {
        long long first = absStep, last = absStep;
        int n = 1;
        while (n < c.len && c.bit (first - 1)) { --first; ++n; }
        while (n < c.len && c.bit (last + 1)) { ++last; ++n; }
        startBeat = c.stepStart (first);
        lengthBeats = n >= c.len ? 1.0e9 : c.stepStart (last + 1) - startBeat;   // every step on: endless
    }

    // Gate events for the samples [0, n) of a block starting at ppq0, with
    // `open` the gate state the slot is in now (updated). Offsets are the
    // first sample at or after each step boundary. Returns the count.
    inline int laneEvents (const LaneClock& c, double ppq0, double beatsPerSample, int n,
                           bool& open, GateEvent* out, int maxOut, int baseOffset = 0)
    {
        int count = 0;
        auto emit = [&] (int off, bool on, double atBeat)
        {
            if (count >= maxOut) return;
            GateEvent e;
            e.offset = baseOffset + off;
            e.on = on;
            if (on)
            {
                double start = 0.0, len = 0.0;
                runAround (c, c.stepAt (atBeat), start, len);
                e.runBeats = len;
                e.phaseBeats = juce::jmax (0.0, atBeat - start);
            }
            out[count++] = e;
            open = on;
        };
        if (n <= 0 || beatsPerSample <= 0.0) return 0;
        const double eps = c.stepBeats * 1.0e-7;
        // the state at the block start (covers host jumps, starts, scene switches)
        if (c.openAt (ppq0 + eps) != open) emit (0, ! open, ppq0 + eps);
        long long k = c.stepAt (ppq0 + eps);
        for (int guard = 0; guard < 4096; ++guard)
        {
            const double nb = c.stepStart (k + 1);
            const int off = (int) std::ceil ((nb - ppq0) / beatsPerSample - 1.0e-9);
            if (off >= n) break;
            ++k;
            const bool now = c.bit (k);
            if (now != open) emit (juce::jmax (0, off), now, nb + eps);
        }
        return count;
    }

    // Euclidean rhythm: `pulses` on steps spread evenly over `len`, rotated
    inline uint32_t euclid (int len, int pulses, int rotate)
    {
        len = juce::jlimit (1, kMaxSteps, len);
        pulses = juce::jlimit (0, len, pulses);
        uint32_t m = 0;
        for (int i = 0; i < len; ++i)
            if (((i * pulses) % len) < pulses) m |= 1u << (uint32_t) (((i + rotate) % len + len) % len);
        return m;
    }
}
