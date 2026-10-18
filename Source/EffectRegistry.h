#pragma once
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

// ============================================================
// Effect registry: every effect in the grid, its category, visualiser kind,
// controls (name / kind / range / default / visibility), modes, default mix
// and default macro routing. Generated from fx_spec.py by
// tools/gen_registry.py — edit the spec, not EffectRegistry.cpp.
// ============================================================

namespace efx
{

enum Category
{
    CatAnalysis = 0, CatDynamics, CatEQ, CatVolume, CatFilter, CatModulation,
    CatStereo, CatPitch, CatDrive, CatSpace, CatSpectral, CatTime, NumCats
};

enum Viz
{
    VizSpectrum = 0, VizScope, VizLoudness, VizStereoScope, VizPitchDet, VizComp,
    VizLimit, VizSidechain, VizOtt, VizGate, VizDenoise, VizExpander, VizSquash,
    VizEq3, VizEq8, VizDeess, VizHum, VizClick, VizAdsr, VizTransient, VizDuck,
    VizTrance, VizGain, VizMultimode, VizSvf, VizComb, VizFormant, VizLadder,
    VizVintage, VizWavetable, VizAllpass, VizAutoPan, VizChorus, VizFlanger,
    VizPhaser, VizWah, VizTremolo, VizRingMod, VizFm, VizVibrato, VizDoppler,
    VizWobble, VizFShift, VizWidth, VizHaas, VizMonoBass, VizFreqShift,
    VizPitchShift, VizPitchCorr, VizHarmonizer, VizVocoder, VizResonator,
    VizDrive, VizSat, VizFuzz, VizPDist, VizWaveShaper, VizWaveFolder, VizWtDrive,
    VizClipper, VizBitcrush, VizMbDrive, VizLofi, VizAdditive, VizEarly, VizRoom,
    VizPlate, VizSpring, VizShimmer, VizAlgo, VizConv, VizSimpleDly, VizPrecDly,
    VizPingPong, VizSpecDly, VizEcho, VizWarp, VizGranDly, VizSpecComp, VizResSupp,
    VizExciter, VizGranular, VizTimeStretch, VizReverse, VizTapeStop, VizStutter,
    VizShuffler, VizSlicer, VizGlitch, VizChordDet, VizHarmRes, VizMbGate, NumViz
};

// how a 0..1 knob position maps to a physical value / display string
//   Hz Ms Sec Ratio Factor  logarithmic
//   BiHz                    bipolar, cubic (fine near zero)
//   everything else         linear
enum class K { Lin, Hz, Ms, Sec, Db, Pct, BiPct, Pan, Ratio, Factor, Semis, Cents, Deg,
               Oct, Int, Vowel, Note, BiHz, Choice };

// Per-slot parameter pool size. Effects use up to this many controls; the
// plugin's parameter layout uses the same pool so IDs stay stable when a
// slot's effect changes.
inline constexpr int kMaxParams = 16;

// Per-slot non-automatable shape data (drawn LFO / duck curves, step
// patterns, EQ bands, thresholds, harmonic bars ...). Its layout is defined
// by each effect (FxImpl::defaultData); the UI edits it, the DSP reads it.
inline constexpr int kDataSize = 128;

struct ParamSpec
{
    const char* name;
    K kind;
    float lo, hi;
    float def;                     // normalised default (0..1)
    const char* suffix;            // unit for K::Lin ("" otherwise)
    const char* const* choices;    // K::Choice labels
    int numChoices;
    int showSrc;                   // -2 always shown, -1 = depends on mode, k = depends on choice param k
    unsigned showMask;             // bit v set = shown when the source's value is v

    bool isChoice() const { return kind == K::Choice; }
};

struct EffectDef
{
    const char* name;
    Category cat;
    Viz viz;
    const char* desc;
    const char* macroName;         // default macro label ("" = effect has no macro)
    ParamSpec p[kMaxParams];
    int numParams;
    const char* const* modes;
    int numModes;
    float mixDef;                  // 0..1
    float macroDepth[kMaxParams];  // default macro routing (normalised delta at 100%)
    bool usesWavetable() const { return viz == VizWavetable || viz == VizWtDrive; }
    bool usesSidechain() const;    // has an "Ext" mode keyed from the sidechain bus
};

extern const EffectDef kEffects[];
extern const int kNumEffects;

// The v2 control layout (state version 2), for migrating old sessions.
struct LegacyDef
{
    const char* name;
    ParamSpec p[8];
    int numParams;
    const char* const* modes;
    int numModes;
};
extern const LegacyDef kLegacyV2[];
extern const int kNumLegacyV2;

inline int numEffects() { return kNumEffects; }
inline const EffectDef& effectDef (int index) { return kEffects[index]; }
// id 0 = empty slot, 1..kNumEffects = kEffects[id-1]
inline const EffectDef* defForId (int id) { return (id >= 1 && id <= kNumEffects) ? &kEffects[id - 1] : nullptr; }
int idForName (juce::StringRef name);   // 0 if unknown

// physical value from a normalised knob position
float unmapParam (const ParamSpec& s, float x01);
// normalised knob position from a physical value (inverse of unmapParam)
float mapParam (const ParamSpec& s, float v);
// display string from a normalised knob position
juce::String formatParam (const ParamSpec& s, float x01);
// display string -> normalised position (for typed-in values); nullopt-style: returns -1 if unparsable
float parseParam (const ParamSpec& s, const juce::String& text);
// is control k relevant given the slot's mode and normalised param values?
bool paramVisible (const EffectDef& d, int k, int mode, const float* norm);
// tempo division label ("1/8D", "1/16T", "2 bars") -> length in beats
double divisionBeats (const char* label);

// category colours (dark tile fill + pastel accent + ink)
juce::Colour catAccent (int cat);
juce::Colour catDim (int cat);
juce::Colour catInk (int cat);
const char* catName (int cat);

} // namespace efx
