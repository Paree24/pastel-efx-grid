#pragma once
#include "Effects.h"
#include "Scales.h"
#include <algorithm>

// Harmonic Resonator: which pitches ring, shared by the DSP and the visual.
// Controls (physical): 0 Root, 1 Scale, 2 Chord, 3 Low, 4 High, 5 Reso,
// 6 Tilt, 7 Drive, 8 Spread, 9 Out, 10 Arp, 11 Rate, 12 Gate, 13 Swing.
// Modes: Scale / Chord / Custom.
// Shape data [0..11]: the Custom set's level per pitch class (0..1).
namespace efx::harmres
{
    inline constexpr int kMaxRes = 128;
    inline constexpr int kModeScale = 0, kModeChord = 1, kModeCustom = 2;

    // level 0..1 of each pitch class (absolute, C = 0) in the current set
    inline void pitchSet (const float* p, int mode, const float* data, float* w12)
    {
        for (int k = 0; k < 12; ++k) w12[k] = 0.0f;
        const int root = juce::jlimit (0, 11, (int) std::lround (p[0]));
        if (mode == kModeCustom)
        {
            for (int k = 0; k < 12; ++k) w12[k] = juce::jlimit (0.0f, 1.0f, data != nullptr ? data[k] : 0.0f);
            return;
        }
        if (mode == kModeChord)
        {
            const auto& q = kChordQualities[juce::jlimit (0, kNumChordQualities - 1, (int) std::lround (p[2]))];
            for (int iv : q.iv) if (iv >= 0) w12[(root + iv) % 12] = 1.0f;
            return;
        }
        const auto& sc = kScales[juce::jlimit (0, kNumScales - 1, (int) std::lround (p[1]))];
        for (int i = 0; i < sc.n; ++i) w12[(root + sc.iv[i]) % 12] = 1.0f;
    }

    // arpeggiator (Midifier's modes): 0 Off, 1 Up, 2 Down, 3 UpDown, 4 DownUp, 5 Random, 6 Played
    inline constexpr int kArpOff = 0, kArpRandom = 5, kArpPlayed = 6;

    // the set's pitch classes in arp order: Played = the set's own order
    // (scale degrees, chord tones root-3rd-5th-7th-9th, custom keys from the
    // root); every other mode starts from them sorted upwards from the root.
    inline int arpPool (const float* p, int mode, const float* data, int arpMode, int* pcs)
    {
        const int root = juce::jlimit (0, 11, (int) std::lround (p[0]));
        int n = 0;
        auto add = [&] (int pc) { pc = ((pc % 12) + 12) % 12; for (int i = 0; i < n; ++i) if (pcs[i] == pc) return; pcs[n++] = pc; };
        if (mode == kModeChord)
        {
            const auto& q = kChordQualities[juce::jlimit (0, kNumChordQualities - 1, (int) std::lround (p[2]))];
            for (int iv : q.iv) if (iv >= 0) add (root + iv);
        }
        else
        {
            float w12[12];
            pitchSet (p, mode, data, w12);
            for (int k = 0; k < 12; ++k) if (w12[(root + k) % 12] > 0.001f) add (root + k);
        }
        if (arpMode != kArpPlayed)   // sort upwards from the root
            std::sort (pcs, pcs + n, [root] (int a, int b) { return (a - root + 12) % 12 < (b - root + 12) % 12; });
        return n;
    }

    // the pitch class arp step `step` plays (Random handled by the caller)
    inline int arpNote (const int* pcs, int n, int arpMode, long long step)
    {
        if (n <= 0) return -1;
        const auto at = [&] (long long i) { return pcs[(size_t) (((i % n) + n) % n)]; };
        switch (arpMode)
        {
            case 2: return pcs[n - 1 - (int) (((step % n) + n) % n)];   // Down
            case 3: { if (n == 1) return pcs[0]; const long long cyc = 2 * n - 2, q = ((step % cyc) + cyc) % cyc;   // UpDown
                      return q < n ? pcs[q] : pcs[cyc - q]; }
            case 4: { if (n == 1) return pcs[0]; const long long cyc = 2 * n - 2, q = ((step % cyc) + cyc) % cyc;   // DownUp
                      return q < n ? pcs[n - 1 - q] : pcs[q - (n - 1)]; }
            default: return at (step);   // Up, Played
        }
    }

    // resonance Q from the Reso control (0..1)
    inline float qOf (float reso) { return 8.0f * std::pow (40.0f, juce::jlimit (0.0f, 1.0f, reso)); }

    // every ringing pitch between Low and High (lowest first): frequency,
    // level (set level x tilt) and pitch class. Returns the count.
    inline int notes (const float* p, int mode, const float* data, float* hz, float* gain, int maxN = kMaxRes, int* pcOut = nullptr)
    {
        float w12[12];
        pitchSet (p, mode, data, w12);
        const float lo = juce::jmax (16.0f, p[3]), hi = juce::jmax (lo * 1.01f, p[4]);
        const float tilt = p[6];
        int n = 0;
        for (int midi = 0; midi < 136 && n < maxN; ++midi)
        {
            const float w = w12[midi % 12];
            if (w <= 0.001f) continue;
            const float f = 440.0f * std::pow (2.0f, (float) (midi - 69) / 12.0f);
            if (f < lo || f > hi) continue;
            hz[n] = f;
            gain[n] = w * std::pow (10.0f, tilt * std::log2 (f / 1000.0f) / 20.0f);
            if (pcOut != nullptr) pcOut[n] = midi % 12;
            ++n;
        }
        return n;
    }

    // summed magnitude of the bank at hz (linear; for drawing)
    inline float magnitude (float hzAt, const float* hz, const float* gain, int n, float q)
    {
        float sum = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float x = hzAt / hz[i] - hz[i] / hzAt;
            sum += gain[i] / std::sqrt (1.0f + q * q * x * x);
        }
        return sum;
    }

    // "C Major", "D maj7", "Custom"
    inline juce::String setName (const float* p, int mode)
    {
        static const char* const keys[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const juce::String root = keys[juce::jlimit (0, 11, (int) std::lround (p[0]))];
        if (mode == kModeCustom) return "Custom";
        if (mode == kModeChord) return root + " " + kChordQualities[juce::jlimit (0, kNumChordQualities - 1, (int) std::lround (p[2]))].name;
        return root + " " + kScales[juce::jlimit (0, kNumScales - 1, (int) std::lround (p[1]))].name;
    }
}
