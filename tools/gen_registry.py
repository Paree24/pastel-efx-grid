#!/usr/bin/env python3
"""Generate Source/EffectRegistry.cpp from fx_spec.py.

fx_spec.py lists every effect's real controls in physical units. Here each
control becomes a ParamSpec with a normalised default (using the same taper as
efx::unmapParam / efx::mapParam in EffectRegistryImpl.cpp), visibility rules
are resolved to (source, bitmask) pairs, and the default macro routing is
turned into per-control normalised depths.

    python3 tools/gen_registry.py
"""
import os, sys, math

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
from fx_spec import EFFECTS  # noqa: E402
sys.path.insert(0, HERE)
import fx_spec_v2  # noqa: E402  frozen v2 layout, for session migration

kMAX = 16
kLEGACY_MAX = 8

CAT_ENUM = {
    "Analysis": "CatAnalysis", "Dynamics": "CatDynamics", "EQ": "CatEQ",
    "Volume": "CatVolume", "Filter": "CatFilter", "Modulation": "CatModulation",
    "Stereo": "CatStereo", "Pitch": "CatPitch", "Drive": "CatDrive",
    "Space": "CatSpace", "Spectral": "CatSpectral", "Time": "CatTime",
}

VIZ_CAMEL = {
    "spectrum": "VizSpectrum", "scope": "VizScope", "loudness": "VizLoudness",
    "stereoscope": "VizStereoScope", "pitchdet": "VizPitchDet", "comp": "VizComp",
    "limit": "VizLimit", "sidechain": "VizSidechain", "ott": "VizOtt",
    "gate": "VizGate", "denoise": "VizDenoise", "expander": "VizExpander",
    "squash": "VizSquash", "eq3": "VizEq3", "eq8": "VizEq8", "deess": "VizDeess",
    "hum": "VizHum", "click": "VizClick", "adsr": "VizAdsr", "transient": "VizTransient",
    "duck": "VizDuck", "trance": "VizTrance", "gain": "VizGain",
    "multimode": "VizMultimode", "svf": "VizSvf", "comb": "VizComb",
    "formant": "VizFormant", "ladder": "VizLadder", "vintage": "VizVintage",
    "wavetable": "VizWavetable", "allpass": "VizAllpass", "autopan": "VizAutoPan",
    "chorus": "VizChorus", "flanger": "VizFlanger", "phaser": "VizPhaser",
    "wah": "VizWah", "tremolo": "VizTremolo", "ringmod": "VizRingMod",
    "fm": "VizFm", "vibrato": "VizVibrato", "doppler": "VizDoppler",
    "wobble": "VizWobble", "fshifter": "VizFShift", "width": "VizWidth",
    "haas": "VizHaas", "monobass": "VizMonoBass", "freqshift": "VizFreqShift",
    "pitchshift": "VizPitchShift", "pitchcorr": "VizPitchCorr",
    "harmonizer": "VizHarmonizer", "vocoder": "VizVocoder", "resonator": "VizResonator",
    "drive": "VizDrive", "sat": "VizSat", "fuzz": "VizFuzz", "pdist": "VizPDist",
    "waveshaper": "VizWaveShaper", "wavefolder": "VizWaveFolder",
    "wtdrive": "VizWtDrive", "clipper": "VizClipper", "bitcrush": "VizBitcrush",
    "mbdrive": "VizMbDrive", "lofi": "VizLofi", "additive": "VizAdditive",
    "early": "VizEarly", "room": "VizRoom", "plate": "VizPlate",
    "spring": "VizSpring", "shimmer": "VizShimmer", "algo": "VizAlgo",
    "conv": "VizConv", "simpledly": "VizSimpleDly", "precdly": "VizPrecDly",
    "pingpong": "VizPingPong", "specdly": "VizSpecDly", "echo": "VizEcho",
    "warp": "VizWarp", "grandly": "VizGranDly", "speccomp": "VizSpecComp",
    "ressupp": "VizResSupp", "exciter": "VizExciter", "granular": "VizGranular",
    "timestretch": "VizTimeStretch", "reverse": "VizReverse", "tapestop": "VizTapeStop",
    "stutter": "VizStutter", "shuffler": "VizShuffler", "slicer": "VizSlicer",
    "glitch": "VizGlitch", "chorddet": "VizChordDet", "harmres": "VizHarmRes", "mbgate": "VizMbGate", "humanizer": "VizHumanizer", "talkbox": "VizTalkbox",
}

LOG_KINDS = ("Hz", "Ms", "Sec", "Ratio", "Factor")
KINDS = ("Lin", "Hz", "Ms", "Sec", "Db", "Pct", "BiPct", "Pan", "Ratio", "Factor", "Semis",
         "Cents", "Deg", "Oct", "Int", "Vowel", "Note", "BiHz", "Choice")


def to01(kind, lo, hi, v):
    """physical -> normalised; must mirror efx::mapParam."""
    v = min(max(v, min(lo, hi)), max(lo, hi))
    if kind in LOG_KINDS:
        if lo > 0:
            return math.log(v / lo) / math.log(hi / lo)
        return math.sqrt((v - lo) / (hi - lo))
    if kind == "BiHz":
        t = v / hi
        return 0.5 + 0.5 * math.copysign(abs(t) ** (1.0 / 3.0), t)
    return (v - lo) / (hi - lo) if hi != lo else 0.0


def cf(x):
    return f"{x:.6f}f"


def cs(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def spec_literal (f, i, k, c, ctrls, labels, modes, arrays, prefix):
    """one ParamSpec initialiser; choice label arrays are appended to `arrays`"""
    src, mask = -2, 0xFFFFFFFF
    if c["show"]:
        what, vals = c["show"]
        if what == "mode":
            src = -1
            bad = [v for v in vals if v not in modes]
            if bad:
                raise SystemExit(f"{f['name']}.{c['label']}: unknown mode {bad}")
            mask = sum(1 << modes.index(v) for v in vals)
        else:
            src = labels.index(what)
            ch = ctrls[src]
            if ch["kind"] != "Choice":
                raise SystemExit(f"{f['name']}.{c['label']}: show source must be a choice")
            mask = sum(1 << ch["choices"].index(v) for v in vals)

    if c["kind"] == "Choice":
        name = f"{prefix}CH_{i}_{k}"
        arrays.append(f"static const char* const {name}[] = {{ "
                      + ", ".join(cs(x) for x in c["choices"]) + " };")
        n = len(c["choices"])
        if not 0 <= c["idx"] < n:
            raise SystemExit(f"{f['name']}.{c['label']}: default index out of range")
        d01 = c["idx"] / max(1, n - 1)
        return (f'{{ {cs(c["label"])}, K::Choice, 0.0f, {cf(n - 1)}, {cf(d01)}, "", '
                f'{name}, {n}, {src}, {mask:#x}u }}')
    kind, suffix = c["kind"], ""
    if ":" in kind:
        kind, suffix = kind.split(":", 1)
    if kind not in KINDS:
        raise SystemExit(f"{f['name']}.{c['label']}: bad kind {kind}")
    lo, hi, dv = c["lo"], c["hi"], c["default"]
    if not (min(lo, hi) <= dv <= max(lo, hi)):
        raise SystemExit(f"{f['name']}.{c['label']}: default {dv} outside {lo}..{hi}")
    c["_kind"] = kind
    return (f'{{ {cs(c["label"])}, K::{kind}, {cf(lo)}, {cf(hi)}, '
            f'{cf(to01(kind, lo, hi, dv))}, {cs(suffix)}, nullptr, 0, {src}, {mask:#x}u }}')


def main():
    out = ["// AUTO-GENERATED by tools/gen_registry.py from fx_spec.py - do not edit by hand.",
           '#include "EffectRegistry.h"', "namespace efx {", ""]
    arrays = []
    effects = []
    names = set()

    for i, f in enumerate(EFFECTS):
        if f["name"] in names:
            raise SystemExit(f"duplicate effect {f['name']}")
        names.add(f["name"])
        ctrls = f["ctrls"]
        if not 1 <= len(ctrls) <= kMAX:
            raise SystemExit(f"{f['name']}: {len(ctrls)} controls (1..{kMAX})")
        labels = [c["label"] for c in ctrls]
        if len(set(labels)) != len(labels):
            raise SystemExit(f"{f['name']}: duplicate control labels")
        modes = f["modes"]
        if len(modes) > 4:
            raise SystemExit(f"{f['name']}: more than 4 modes")
        if modes:
            arrays.append(f"static const char* const M_{i}[] = {{ " + ", ".join(cs(m) for m in modes) + " };")

        specs = [spec_literal (f, i, k, c, ctrls, labels, modes, arrays, "") for k, c in enumerate(ctrls)]

        # default macro routing -> normalised depth per control
        depth = [0.0] * kMAX
        macro_name = ""
        if f["macro"]:
            macro_name, targets = f["macro"]
            for lbl, tgt in targets.items():
                if lbl not in labels:
                    raise SystemExit(f"{f['name']}: macro target {lbl} is not a control")
                c = ctrls[labels.index(lbl)]
                if c["kind"] == "Choice":
                    raise SystemExit(f"{f['name']}: macro cannot drive choice {lbl}")
                k = c["_kind"]
                depth[labels.index(lbl)] = to01(k, c["lo"], c["hi"], tgt) - to01(k, c["lo"], c["hi"], c["default"])
                if abs(depth[labels.index(lbl)]) < 0.02:
                    raise SystemExit(f"{f['name']}: macro target {lbl} barely moves")
        elif f["cat"] != "Analysis":
            raise SystemExit(f"{f['name']}: audio effects need a default macro")

        while len(specs) < kMAX:
            specs.append('{ "", K::Lin, 0.0f, 1.0f, 0.0f, "", nullptr, 0, -2, 0xffffffffu }')

        effects.append(
            f'    {{ {cs(f["name"])}, {CAT_ENUM[f["cat"]]}, {VIZ_CAMEL[f["viz"]]},\n'
            f'      {cs(f["desc"])}, {cs(macro_name)},\n'
            f'      {{ ' + ",\n        ".join(specs) + ' },\n'
            f'      {len(ctrls)}, ' + (f"M_{i}, {len(modes)}" if modes else "nullptr, 0")
            + f', {cf(f["mix"])},\n'
            f'      {{ ' + ", ".join(cf(d) for d in depth) + ' } },')

    # legacy v2 layout (for migrating version-2 sessions by control name)
    legacy = []
    for i, f in enumerate(fx_spec_v2.EFFECTS):
        ctrls = f["ctrls"]
        labels = [c["label"] for c in ctrls]
        modes = f["modes"]
        if modes:
            arrays.append(f"static const char* const LM_{i}[] = {{ " + ", ".join(cs(m) for m in modes) + " };")
        sp = [spec_literal (f, i, k, c, ctrls, labels, modes, arrays, "L") for k, c in enumerate(ctrls)]
        while len(sp) < kLEGACY_MAX:
            sp.append('{ "", K::Lin, 0.0f, 1.0f, 0.0f, "", nullptr, 0, -2, 0xffffffffu }')
        legacy.append(f'    {{ {cs(f["name"])}, {{ ' + ", ".join(sp) + f' }}, {len(ctrls)}, '
                      + (f"LM_{i}, {len(modes)}" if modes else "nullptr, 0") + " },")

    out += arrays
    out += ["", "extern const EffectDef kEffects[] = {"] + effects + ["};",
            f"extern const int kNumEffects = {len(EFFECTS)};", "",
            "extern const LegacyDef kLegacyV2[] = {"] + legacy + ["};",
            f"extern const int kNumLegacyV2 = {len(fx_spec_v2.EFFECTS)};", "", "} // namespace efx", ""]
    # effect ids are positional (saved in sessions): the v2 effects must keep their places,
    # except deliberate replacements whose old sessions the processor migrates
    # (EfxGridProcessor::setStateInformation, state version 4: ADSR -> Transient Shaper)
    REPLACED = {"ADSR": "Harmonic Resonator"}
    for i, f in enumerate(fx_spec_v2.EFFECTS):
        if EFFECTS[i]["name"] != f["name"] and REPLACED.get(f["name"]) != EFFECTS[i]["name"]:
            raise SystemExit(f"effect #{i + 1} changed from {f['name']} to {EFFECTS[i]['name']}: append new effects at the end")
    dst = os.path.join(ROOT, "Source", "EffectRegistry.cpp")
    with open(dst, "w") as fh:
        fh.write("\n".join(out))
    print(f"wrote {dst}: {len(EFFECTS)} effects")


if __name__ == "__main__":
    main()
