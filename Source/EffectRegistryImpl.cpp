#include "EffectRegistry.h"
#include <cmath>

// Non-generated half of the effect registry: parameter tapers, display and
// parsing, visibility rules and the per-category pastel palette. The tapers
// must mirror to01() in tools/gen_registry.py.

namespace efx
{

namespace
{
    bool isLogKind (K k) { return k == K::Hz || k == K::Ms || k == K::Sec || k == K::Ratio || k == K::Factor; }

    juce::String num (float v, int decimals)
    {
        if (decimals <= 0) return juce::String (juce::roundToInt (v));
        return juce::String (v, decimals);
    }

    juce::String signedNum (float v, int decimals)
    {
        const auto s = num (v, decimals);
        return (v > 0.0f && s.getFloatValue() != 0.0f) ? "+" + s : s;
    }

    const char* const kNoteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char* const kVowels[] = { "A", "E", "I", "O", "U" };
}

float unmapParam (const ParamSpec& s, float x01)
{
    x01 = juce::jlimit (0.0f, 1.0f, x01);
    switch (s.kind)
    {
        case K::Choice:
            return (float) std::lround (x01 * (float) juce::jmax (0, s.numChoices - 1));
        case K::Int:
        case K::Semis:
            return std::round (s.lo + (s.hi - s.lo) * x01);
        case K::BiHz:
        {
            const float t = 2.0f * x01 - 1.0f;
            return s.hi * t * t * t;
        }
        default:
            break;
    }
    if (isLogKind (s.kind))
    {
        if (s.lo > 0.0f && s.hi > s.lo) return s.lo * std::pow (s.hi / s.lo, x01);
        return s.lo + (s.hi - s.lo) * x01 * x01;
    }
    return s.lo + (s.hi - s.lo) * x01;
}

float mapParam (const ParamSpec& s, float v)
{
    switch (s.kind)
    {
        case K::Choice:
        {
            const int nc = juce::jmax (2, s.numChoices);
            return juce::jlimit (0.0f, 1.0f, (float) std::lround (v) / (float) (nc - 1));
        }
        case K::BiHz:
        {
            const float t = juce::jlimit (-1.0f, 1.0f, v / s.hi);
            return 0.5f + 0.5f * std::copysign (std::cbrt (std::abs (t)), t);
        }
        default:
            break;
    }
    if (s.hi == s.lo) return 0.0f;
    v = juce::jlimit (juce::jmin (s.lo, s.hi), juce::jmax (s.lo, s.hi), v);
    if (isLogKind (s.kind))
    {
        if (s.lo > 0.0f) return juce::jlimit (0.0f, 1.0f, std::log (v / s.lo) / std::log (s.hi / s.lo));
        return juce::jlimit (0.0f, 1.0f, std::sqrt ((v - s.lo) / (s.hi - s.lo)));
    }
    return juce::jlimit (0.0f, 1.0f, (v - s.lo) / (s.hi - s.lo));
}

static juce::String formatHz (float v)
{
    const float a = std::abs (v);
    if (a >= 1000.0f) return num (v / 1000.0f, a >= 10000.0f ? 1 : 2) + " kHz";
    if (a < 1.0f)     return num (v, 2) + " Hz";
    return num (v, a < 100.0f ? 1 : 0) + " Hz";
}

static juce::String formatMs (float v)
{
    if (v >= 1000.0f) return num (v / 1000.0f, 2) + " s";
    if (v < 1.0f)     return num (v, 2) + " ms";
    return num (v, v < 10.0f ? 1 : 0) + " ms";
}

juce::String formatParam (const ParamSpec& s, float x01)
{
    if (s.kind == K::Choice)
    {
        const int idx = (int) unmapParam (s, x01);
        return (s.choices != nullptr && idx >= 0 && idx < s.numChoices) ? juce::String (s.choices[idx]) : "-";
    }

    const float v = unmapParam (s, x01);
    switch (s.kind)
    {
        case K::Hz:     return formatHz (v);
        case K::BiHz:   return (v > 0.05f ? "+" : "") + formatHz (v);
        case K::Ms:     return formatMs (v);
        case K::Sec:    return v < 1.0f ? formatMs (v * 1000.0f) : num (v, v < 10.0f ? 2 : 1) + " s";
        case K::Db:
            if (s.lo <= -79.0f && v <= s.lo + 0.05f) return "-inf dB";
            return signedNum (v, std::abs (v) < 10.0f ? 1 : 0) + " dB";
        case K::Pct:    return num (v * 100.0f, 0) + "%";
        case K::BiPct:  return signedNum (v * 100.0f, 0) + "%";
        case K::Pan:
        {
            const int p = juce::roundToInt (v * 100.0f);
            return p == 0 ? juce::String ("C") : (p < 0 ? "L" + juce::String (-p) : "R" + juce::String (p));
        }
        case K::Ratio:  return num (v, v < 10.0f ? 1 : 0) + ":1";
        case K::Factor: return num (v, 2) + "x";
        case K::Semis:  return signedNum (v, 0) + " st";
        case K::Cents:  return signedNum (v, 0) + " ct";
        case K::Deg:    return num (v, 0) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
        case K::Oct:    return signedNum (v, 1) + " oct";
        case K::Int:    return num (v, 0);
        case K::Vowel:
        {
            const int i0 = juce::jlimit (0, 4, (int) std::floor (v));
            const float fr = v - (float) i0;
            if (i0 >= 4 || fr < 0.12f) return kVowels[i0];
            if (fr > 0.88f) return kVowels[i0 + 1];
            return juce::String (kVowels[i0]) + ">" + kVowels[i0 + 1];
        }
        case K::Note:
        {
            const int m = juce::roundToInt (v);
            return juce::String (kNoteNames[((m % 12) + 12) % 12]) + juce::String (m / 12 - 1);
        }
        case K::Lin:
        {
            const float range = std::abs (s.hi - s.lo);
            const int dec = range <= 2.0f ? 2 : (range <= 30.0f ? 1 : 0);
            const juce::String suf (s.suffix != nullptr ? s.suffix : "");
            const auto n = (s.lo < 0.0f ? signedNum (v, dec) : num (v, dec));
            return suf.isEmpty() ? n : n + " " + suf;
        }
        default:        return num (v, 2);
    }
}

float parseParam (const ParamSpec& s, const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty()) return -1.0f;

    if (s.kind == K::Choice)
    {
        for (int i = 0; i < s.numChoices; ++i)
            if (t.equalsIgnoreCase (s.choices[i])) return mapParam (s, (float) i);
        return -1.0f;
    }
    if (s.kind == K::Note)
    {
        for (int i = 11; i >= 0; --i)
            if (t.startsWithIgnoreCase (kNoteNames[i]))
            {
                const int oct = t.substring ((int) strlen (kNoteNames[i])).getIntValue();
                return mapParam (s, (float) ((oct + 1) * 12 + i));
            }
    }
    if (s.kind == K::Pan)
    {
        if (t.equalsIgnoreCase ("c")) return 0.5f;
        const float a = t.substring (1).getFloatValue() / 100.0f;
        if (t.startsWithIgnoreCase ("l")) return mapParam (s, -a);
        if (t.startsWithIgnoreCase ("r")) return mapParam (s, a);
    }

    float v = t.retainCharacters ("+-.0123456789").getFloatValue();
    const auto lower = t.toLowerCase();
    if (lower.contains ("khz") || (lower.endsWith ("k") && s.kind == K::Hz)) v *= 1000.0f;
    if (s.kind == K::Ms && lower.endsWith (" s")) v *= 1000.0f;
    if (s.kind == K::Sec && lower.contains ("ms")) v *= 0.001f;
    if (s.kind == K::Pct || s.kind == K::BiPct || s.kind == K::Pan) v /= 100.0f;
    return mapParam (s, v);
}

bool paramVisible (const EffectDef& d, int k, int mode, const float* norm)
{
    if (k < 0 || k >= d.numParams) return false;
    const auto& s = d.p[k];
    if (s.showSrc == -2) return true;
    int v = 0;
    if (s.showSrc == -1) v = mode;
    else if (s.showSrc >= 0 && s.showSrc < d.numParams && norm != nullptr) v = (int) unmapParam (d.p[s.showSrc], norm[s.showSrc]);
    return v >= 0 && v < 32 && ((s.showMask >> v) & 1u) != 0;
}

bool EffectDef::usesSidechain() const
{
    for (int i = 0; i < numModes; ++i)
        if (juce::String (modes[i]) == "Ext") return true;
    return false;
}

int idForName (juce::StringRef name)
{
    for (int i = 0; i < kNumEffects; ++i)
        if (name == juce::StringRef (kEffects[i].name)) return i + 1;
    return 0;
}

double divisionBeats (const char* label)
{
    juce::String s (label);
    if (s.endsWith ("bars") || s.endsWith ("bar"))
    {
        const auto first = s.upToFirstOccurrenceOf (" ", false, false);
        if (first.contains ("/"))
            return 4.0 * first.upToFirstOccurrenceOf ("/", false, false).getDoubleValue()
                       / juce::jmax (1.0, first.fromFirstOccurrenceOf ("/", false, false).getDoubleValue());
        return 4.0 * juce::jmax (1.0, first.getDoubleValue());
    }
    double mult = 1.0;
    if (s.endsWithIgnoreCase ("D")) { mult = 1.5; s = s.dropLastCharacters (1); }
    else if (s.endsWithIgnoreCase ("T")) { mult = 2.0 / 3.0; s = s.dropLastCharacters (1); }
    const double num = s.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
    const double den = s.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
    if (den <= 0.0) return 1.0;
    return 4.0 * num / den * mult;      // 1/4 = one beat
}

namespace
{
    struct Palette { juce::uint32 accent, dim, ink; const char* name; };
    const Palette kPalette[NumCats] = {
        { 0xFF9EB9F3u, 0xFF859BCCu, 0xFF2A2F3Cu, "Analysis"   },
        { 0xFFE88A8Au, 0xFFC47474u, 0xFF3A2A2Eu, "Dynamics"   },
        { 0xFFE8B86Au, 0xFFC29B5Bu, 0xFF372F23u, "EQ"         },
        { 0xFFE6DC9Au, 0xFFBFB57Eu, 0xFF37351Fu, "Volume"     },
        { 0xFF8FD0C2u, 0xFF79AEA3u, 0xFF273333u, "Filter"     },
        { 0xFFB9A7F0u, 0xFF9B8DC9u, 0xFF2E2C3Cu, "Modulation" },
        { 0xFF8FC6E8u, 0xFF79A6C3u, 0xFF27323Au, "Stereo"     },
        { 0xFFE9A3B8u, 0xFFC3899Bu, 0xFF372B32u, "Pitch"      },
        { 0xFFF2B48Fu, 0xFFCA977Au, 0xFF392E2Au, "Drive"      },
        { 0xFF86C5C5u, 0xFF6FA8A8u, 0xFF243535u, "Space"      },
        { 0xFFA9D58Eu, 0xFF8DB476u, 0xFF2C3524u, "Spectral"   },
        { 0xFFC3B2E8u, 0xFFA295C2u, 0xFF322C40u, "Time"       },
    };
}

juce::Colour catAccent (int cat) { return juce::Colour (kPalette[juce::jlimit (0, (int) NumCats - 1, cat)].accent); }
juce::Colour catDim    (int cat) { return juce::Colour (kPalette[juce::jlimit (0, (int) NumCats - 1, cat)].dim); }
juce::Colour catInk    (int cat) { return juce::Colour (kPalette[juce::jlimit (0, (int) NumCats - 1, cat)].ink); }
const char*  catName   (int cat) { return kPalette[juce::jlimit (0, (int) NumCats - 1, cat)].name; }

} // namespace efx
