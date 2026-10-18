# Pastel EFX Grid

A JUCE audio plugin (VST3 + Standalone) that presents a **4×4 grid of 16 effect
slots**. Each slot can load any of **89 effects**, in the dark/pastel theme from
the original HTML/SVG mockup (`grid.svg`, `grid.html`, `all-effects.html`).

The 16 slots are chained in series, left-to-right, top-to-bottom. Every slot has
an effect, a mode, up to eight controls of its own, a user-routable macro, a
dry/wet mix and a click-free bypass.

Effects **fill the slots in order**: adding an effect appends it to the first
free slot, so the chain is always packed with no gaps. Removing a slot shifts the
later effects up; dragging a tile onto another moves it there. Moves and removals
carry the whole slot state (values, mode, mix, bypass, macro routing, wavetable).

## Using it

- **Add**: `+ ADD EFFECT`, a category pill, or click an empty tile.
- **Open** a tile to see every control, the live visualiser, mode, mix and
  macro routing. Controls that do not apply in the current mode (e.g. *Time*
  while *Sync* is on) are dimmed. Type values straight into a knob's readout.
- **Tiles**: the power icon bypasses; right-click for move / reset / replace /
  remove; drag to reorder.
- **Perform pads**: drag left/right to play that slot's macro.
- **Macro routing** is per slot and user-chosen. Each effect starts with a
  sensible default routing (shown under *MACRO ROUTING*). Press **ASSIGN** and
  drag any knob to set how far the macro pushes it (up or down, double-click
  clears), or right-click a knob for quick options. **DEFAULT** restores the
  effect's routing. The outer arc on a knob shows its macro range; the white dot
  shows its live, macro-modulated value.
- **Wavetables**: the *Wavetable Filter* and *Wavetable Drive* load WAV/AIFF/FLAC
  tables (`LOAD TABLE...`). Files whose length is a multiple of 2048 samples are
  sliced into 2048-sample frames (the common multi-frame layout); short files are one cycle. The
  table is embedded in the session (up to 64 frames, 16-bit).
- **Sidechain**: the plugin has an optional stereo sidechain bus. *Sidechain
  Comp*, *Ducker* and *Vocoder* use it in their *Ext* mode (falling back to the
  input when nothing is connected).

## Building

JUCE is taken from the sibling checkout by default. Requires CMake ≥ 3.22 and a
C++17 compiler.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPASTEL_JUCE_DIR=/home/paree/ai_experiments/Nanoberg/JUCE
cmake --build build
```

Override `PASTEL_JUCE_DIR` to point at another JUCE 8 checkout, or omit it to let
CMake fetch JUCE 8.0.6 from git.

Artefacts:

- `build/PastelEfxGrid_artefacts/Release/VST3/Pastel EFX Grid.vst3`
- `build/PastelEfxGrid_artefacts/Release/Standalone/Pastel EFX Grid`

## Editing the effects

`fx_spec.py` is the single source of truth for every effect: its controls (kind,
range, default, when they apply), modes, default mix and default macro routing.
After editing it run

```sh
python3 tools/gen_registry.py     # regenerates Source/EffectRegistry.cpp
```

The generator validates the spec (defaults inside ranges, macro targets exist and
actually move, visibility rules reference real modes / choices). Each effect's DSP
is an `FxImpl` class in `Source/dsp/fx/*.cpp`, reading its controls in the order
`fx_spec.py` lists them. (`fx_data.py` only feeds the HTML/SVG mockup scripts.)

## Headless self-test

```sh
cmake --build build --target GridTest && ./build/GridTest   # exit 0 = all good
```

- **stability** — every effect at defaults / all-min / all-max / random settings,
  odd block sizes: output stays finite and bounded;
- **flow / level** — at defaults each effect passes signal at a sane level and
  doesn't peak far over full scale;
- **dead controls** — every control, mode and default macro routing must change
  the output (each control is tried in several contexts, with the mode / choice
  it depends on switched on);
- **mix / bypass** — mix 0 and bypass return the dry signal bit-exactly;
- **accuracy** — known-answer checks (frequency shifter, pitch shifter / correction,
  harmonizer, formant shifter, ring mod, compressor curve, limiter ceiling, EQ,
  gain, vibrato depth, synced delay time, EBU R128 loudness);
- **cpu** — real-time factor of the heaviest effects.

## Offscreen UI renderer

`EditorShot` renders the editor to PNGs without a window and checks the slot
model (moves keep settings, sessions round-trip including wavetables):

```sh
cmake --build build --target EditorShot
./build/EditorShot /tmp/shots          # grid, min-size layout, 16 panels
./build/EditorShot /tmp/shots all      # + a panel for every effect
```

## Layout

```
fx_spec.py                   effect specification (source of truth)
Source/
  EffectRegistry.h/.cpp      generated registry (do not edit the .cpp)
  EffectRegistryImpl.cpp     tapers, formatting / parsing, visibility, palette
  PluginProcessor.h/.cpp     APVTS (16 slots), slot model, macro routing, wavetables, state
  PluginEditor.h/.cpp        tiles, expanded panel, controls, macro pads, effect menus
  Visuals.h/.cpp             live per-effect visualisers
  Theme.h/.cpp               PastelLookAndFeel + Lato fonts
  dsp/
    Dsp.h                    DSP primitives (biquads, SVF, ladder, LR4, band splitter,
                             FDN, Dattorro plate, YIN, PSOLA, Hilbert, oversampler…)
    Effects.h/.cpp           GridEffect (smoothing, macro, mix, bypass, tempo) + Wavetable
    fx/*.cpp                 one FxImpl per effect, grouped by category
tools/gen_registry.py        fx_spec.py -> Source/EffectRegistry.cpp
TestGrid.cpp                 headless DSP self-test
TestShot.cpp                 offscreen editor renderer + slot-model checks
```

### Per-slot parameters

For slot `N` (0-based IDs, shown to hosts as `S01 …` to `S16 …`):

| id               | type   | notes                                         |
|------------------|--------|-----------------------------------------------|
| `sN_fx`          | choice | 0 = Empty, 1..89 = effect                     |
| `sN_mode`        | choice | 4 entries, clamped to the effect's mode count |
| `sN_p0`..`sN_p7` | float  | 0..1, the effect's controls (unused ones idle) |
| `sN_macro`       | float  | 0..1                                          |
| `sN_mix`         | float  | dry/wet, 0..1                                 |
| `sN_bypass`      | bool   | faded over ~10 ms                             |

Hosts see each knob's value formatted for the effect in that slot. Macro routing
and wavetables are not automatable; they are saved in the plugin state.
Sessions from the previous version keep their effects and mix; their control
values are reset to the new defaults, because the control layouts changed.

Effect instances are built on the message thread when `sN_fx` changes; the old
instance is kept alive ~1 s before being freed so the audio thread never touches
freed memory. Parameters glide over ~25 ms (processed in 32-sample sub-blocks).

### Latency

The plugin reports zero latency. Most effects add none. The exceptions are
inherent to the algorithm and are not compensated:

- PSOLA effects (*Pitch Correction*, *Harmonizer*, *Formant Shifter*) lag by
  about two pitch periods (~5–25 ms);
- *Pitch Shifter* lags by about its *Window*;
- the oversampled *Drive* effects lag by 1–2 samples.
