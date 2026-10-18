#include "Presets.h"
#include "PluginProcessor.h"
#include "dsp/Scales.h"

namespace presets
{
namespace
{
    // one effect of a factory chain: physical values by control name, mode, mix (-1 = default)
    struct Fx { const char* name; std::vector<std::pair<juce::String, float>> set; int mode = 0; float mix = -1.0f; };
    struct Factory { const char* name; const char* tag; std::vector<Fx> chain; };

    float scaleIdx (const char* n)
    {
        for (int i = 0; i < efx::kNumScales; ++i) if (juce::String (efx::kScales[i].name) == n) return (float) i;
        return 0.0f;
    }
    float chordIdx (const char* n)
    {
        for (int i = 0; i < efx::kNumChordQualities; ++i) if (juce::String (efx::kChordQualities[i].name) == n) return (float) i;
        return 0.0f;
    }

    // Factory chains: common starting points (mastering, mix bus, vocal and
    // drum chains, analysis suites ...) with gentle, mix-ready settings, for music.
    // Level changes stay within a few dB and dynamics are kept (checked by
    // EditorShot's factory audio test).
    const std::vector<Factory>& factory()
    {
        static const std::vector<Factory> f = {
            { "Init", "Init", {} },

            // ---- analysis
            { "Analyzer Suite", "Analyze", {
                { "Spectrum Analyzer", {} },
                { "Loudness Meter", { { "Target", -14.0f } }, 1 },
                { "Stereo Scope", {} },
                { "Oscilloscope", { { "Auto Gain", 1.0f } }, 1 } } },
            { "Tuning & Harmony", "Analyze", {
                { "Pitch Analysis", {} },
                { "Chord Detector", {} },
                { "Spectrum Analyzer", { { "Peak", 1.0f } }, 1 } } },
            { "Loudness & Stereo", "Analyze", {
                { "Loudness Meter", { { "Target", -14.0f } }, 2 },
                { "Stereo Scope", { { "Persist", 0.7f } } } } },

            // ---- mastering (EQ > dynamics > imaging > maximizer, then a meter)
            { "Master: Transparent", "Master", {
                { "3-Band EQ", { { "Low", 0.5f }, { "Low Freq", 50.0f }, { "High", 1.0f }, { "High Freq", 12000.0f } } },
                { "Compressor", { { "Thresh", -12.0f }, { "Ratio", 1.5f }, { "Attack", 30.0f }, { "Release", 150.0f }, { "Knee", 12.0f } }, 1 },
                { "Stereo Width", { { "Width", 1.05f }, { "Bass Mono", 100.0f } } },
                { "Limiter", { { "Input", 2.0f }, { "Ceiling", -1.0f }, { "Release", 80.0f } } },
                { "Loudness Meter", { { "Target", -14.0f } }, 1 } } },
            { "Master: Modern Loud", "Master", {
                { "3-Band EQ", { { "Low", 1.0f }, { "Low Freq", 60.0f }, { "Mid", -0.8f }, { "Mid Freq", 400.0f }, { "Mid Q", 0.7f },
                                 { "High", 1.5f }, { "High Freq", 10000.0f } } },
                { "Multiband Dynamics", { { "Depth", 0.12f }, { "Time", 0.5f } } },
                { "Harmonic Exciter", { { "Amount", 0.15f }, { "Freq", 6000.0f } } },
                { "Stereo Width", { { "Width", 1.1f }, { "Bass Mono", 120.0f } } },
                { "Clipper", { { "Input", 1.0f }, { "Ceiling", -0.6f }, { "Knee", 0.6f } } },
                { "Limiter", { { "Input", 3.0f }, { "Ceiling", -1.0f }, { "Release", 60.0f } } },
                { "Loudness Meter", { { "Target", -9.0f } }, 1 } } },
            { "Master: Warm Analog", "Master", {
                { "Saturation", { { "Drive", 2.0f }, { "Tone", -0.1f } }, 1 },
                { "3-Band EQ", { { "Low", 0.8f }, { "Low Freq", 80.0f }, { "High", 0.8f }, { "High Freq", 9000.0f } } },
                { "Compressor", { { "Thresh", -14.0f }, { "Ratio", 2.0f }, { "Attack", 30.0f }, { "Release", 300.0f }, { "Knee", 9.0f } }, 1 },
                { "Limiter", { { "Input", 2.0f }, { "Ceiling", -1.0f }, { "Release", 120.0f } } },
                { "Loudness Meter", { { "Target", -12.0f } }, 1 } } },

            // ---- mix bus
            { "Mix Bus Glue", "Mix Bus", {
                { "Compressor", { { "Thresh", -16.0f }, { "Ratio", 2.0f }, { "Attack", 30.0f }, { "Release", 100.0f }, { "Knee", 6.0f } }, 1 },
                { "Saturation", { { "Drive", 1.5f } }, 1 } } },
            { "Drum Bus", "Mix Bus", {
                { "Compressor", { { "Thresh", -18.0f }, { "Ratio", 4.0f }, { "Attack", 30.0f }, { "Release", 80.0f }, { "Knee", 6.0f }, { "Makeup", 4.0f } }, 0, 0.5f },
                { "Saturation", { { "Drive", 2.5f } }, 1 },
                { "3-Band EQ", { { "Low", 1.0f }, { "Low Freq", 70.0f }, { "High", 1.0f }, { "High Freq", 9000.0f } } } } },

            // ---- drums
            { "Drum Punch", "Drums", {
                { "Transient Shaper", { { "Attack", 0.3f }, { "Sustain", -0.1f }, { "Decay", 25.0f }, { "Release", 300.0f } } },
                { "3-Band EQ", { { "Low", 2.0f }, { "Low Freq", 70.0f }, { "High", 1.5f }, { "High Freq", 8000.0f } } },
                { "Saturation", { { "Drive", 3.0f } }, 1 },
                { "Clipper", { { "Input", 1.5f }, { "Ceiling", -0.3f }, { "Knee", 0.4f } } } } },
            { "Kick Thump", "Drums", {
                { "Transient Shaper", { { "Attack", 0.25f }, { "Sustain", -0.15f }, { "Decay", 20.0f } } },
                { "3-Band EQ", { { "Low", 2.5f }, { "Low Freq", 60.0f }, { "Mid", -2.5f }, { "Mid Freq", 350.0f }, { "Mid Q", 1.0f } } },
                { "Saturation", { { "Drive", 2.5f } }, 0 } } },
            { "Snare Crack", "Drums", {
                { "Transient Shaper", { { "Attack", 0.2f }, { "Sustain", 0.0f }, { "Decay", 15.0f } } },
                { "3-Band EQ", { { "Mid", 1.0f }, { "Mid Freq", 220.0f }, { "Mid Q", 1.0f }, { "High", 1.0f }, { "High Freq", 6000.0f } } },
                { "Plate Reverb", { { "Decay", 1.2f }, { "Predelay", 10.0f } }, 0, 0.1f },
                { "Clipper", { { "Ceiling", -1.0f }, { "Knee", 0.5f } } } } },
            { "Parallel Smash", "Drums", {
                { "Compressor", { { "Thresh", -28.0f }, { "Ratio", 8.0f }, { "Attack", 2.0f }, { "Release", 60.0f }, { "Makeup", 10.0f } }, 0, 0.25f },
                { "Saturation", { { "Drive", 2.0f } }, 1 } } },
            { "Room Drums", "Drums", {
                { "Transient Shaper", { { "Attack", 0.2f }, { "Sustain", 0.0f } } },
                { "Room Reverb", { { "Size", 0.35f }, { "Decay", 0.6f } }, 0, 0.2f } } },

            // ---- vocals
            { "Vocal: Pop Lead", "Vocal", {
                { "3-Band EQ", { { "Low", -3.0f }, { "Low Freq", 110.0f }, { "Mid", -1.5f }, { "Mid Freq", 350.0f }, { "Mid Q", 1.0f },
                                 { "High", 2.5f }, { "High Freq", 10000.0f } } },
                { "De-Esser", { { "Thresh", -30.0f }, { "Range", 6.0f } } },
                { "Compressor", { { "Thresh", -18.0f }, { "Ratio", 3.0f }, { "Attack", 5.0f }, { "Release", 80.0f }, { "Makeup", 3.0f } } },
                { "Plate Reverb", { { "Decay", 1.6f }, { "Predelay", 30.0f } }, 0, 0.12f },
                { "Simple Delay", { { "Feedback", 0.2f }, { "Lowcut", 300.0f }, { "Tone", 5000.0f } }, 0, 0.08f } } },
            { "Vocal: Upfront Rap", "Vocal", {
                { "3-Band EQ", { { "Low", -3.0f }, { "Low Freq", 120.0f }, { "Mid", 1.5f }, { "Mid Freq", 3000.0f }, { "Mid Q", 0.8f },
                                 { "High", 1.5f }, { "High Freq", 10000.0f } } },
                { "Compressor", { { "Thresh", -20.0f }, { "Ratio", 4.0f }, { "Attack", 3.0f }, { "Release", 60.0f }, { "Makeup", 4.0f } } },
                { "Saturation", { { "Drive", 2.0f } }, 0 },
                { "De-Esser", { { "Thresh", -32.0f }, { "Range", 6.0f } } } } },
            { "Vocal: Airy Ballad", "Vocal", {
                { "3-Band EQ", { { "Low", -2.0f }, { "Low Freq", 100.0f }, { "High", 2.0f }, { "High Freq", 12000.0f } } },
                { "Compressor", { { "Thresh", -18.0f }, { "Ratio", 2.5f }, { "Attack", 10.0f }, { "Release", 120.0f }, { "Makeup", 2.0f } }, 1 },
                { "Harmonic Exciter", { { "Amount", 0.2f }, { "Freq", 8000.0f } } },
                { "Algorithmic Verb", { { "Decay", 2.8f }, { "Predelay", 40.0f } }, 0, 0.18f },
                { "Ping-Pong Delay", { { "Feedback", 0.3f }, { "Lowcut", 400.0f } }, 0, 0.08f } } },
            { "Vocal: Double", "Vocal", {
                { "Haas Effect", { { "Delay", 12.0f }, { "Level", 0.7f } } },
                { "Chorus", { { "Depth", 0.25f }, { "Voices", 2.0f } }, 0, 0.3f } } },

            // ---- bass
            { "Bass: Tight Sub", "Bass", {
                { "Mono Bass", { { "Xover", 120.0f } } },
                { "Compressor", { { "Thresh", -18.0f }, { "Ratio", 4.0f }, { "Attack", 10.0f }, { "Release", 100.0f }, { "Makeup", 3.0f } } },
                { "3-Band EQ", { { "Low", 1.5f }, { "Low Freq", 60.0f }, { "Mid", -2.0f }, { "Mid Freq", 300.0f } } },
                { "Saturation", { { "Drive", 2.0f } }, 1 } } },
            { "Bass: Color", "Bass", {
                { "Harmonic Resonator", { { "Scale", scaleIdx ("Minor Pentatonic") }, { "Root", 9.0f }, { "Low", 35.0f }, { "High", 2500.0f },
                                          { "Reso", 0.55f }, { "Drive", 0.15f }, { "Out", -4.0f } }, 0, 0.35f },
                { "Saturation", { { "Drive", 1.5f } }, 1 },
                { "Mono Bass", { { "Xover", 140.0f } } } } },
            { "Bass: Growl", "Bass", {
                { "Multiband Drive", { { "Low", 2.0f }, { "Mid", 8.0f }, { "High", 4.0f }, { "Out", -1.0f } } },
                { "Ladder Filter", { { "Cutoff", 4000.0f }, { "Reso", 0.25f }, { "Drive", 2.0f } } },
                { "Compressor", { { "Thresh", -16.0f }, { "Ratio", 3.0f }, { "Attack", 10.0f }, { "Release", 80.0f } } } } },

            // ---- guitar / keys / synth
            { "Guitar: Clean Sparkle", "Guitar", {
                { "Compressor", { { "Thresh", -20.0f }, { "Ratio", 3.0f }, { "Attack", 10.0f }, { "Release", 120.0f }, { "Makeup", 3.0f } } },
                { "Chorus", { { "Depth", 0.3f } }, 0, 0.3f },
                { "Plate Reverb", { { "Decay", 1.8f } }, 0, 0.15f } } },
            { "Guitar: Amp Crunch", "Guitar", {
                { "Soft Clip Drive", { { "Drive", 16.0f }, { "Pre HP", 100.0f }, { "Tone", -0.2f }, { "Out", -3.0f } } },
                { "Vintage Filter", { { "Cutoff", 4500.0f }, { "Reso", 0.2f } } },
                { "3-Band EQ", { { "Mid", 1.5f }, { "Mid Freq", 1200.0f } } },
                { "Spring Reverb", { { "Decay", 1.6f } }, 0, 0.15f } } },
            { "Keys: Vintage EP", "Keys", {
                { "Tremolo", { { "Rate", 4.5f }, { "Depth", 0.3f }, { "Stereo", 90.0f } } },
                { "Chorus", { { "Depth", 0.25f } }, 0, 0.25f },
                { "Spring Reverb", { { "Decay", 1.5f } }, 0, 0.12f } } },
            { "Synth: Wide Pad", "Keys", {
                { "Chorus", { { "Depth", 0.4f }, { "Voices", 3.0f } }, 0, 0.4f },
                { "Stereo Width", { { "Width", 1.3f }, { "Bass Mono", 150.0f } } },
                { "Algorithmic Verb", { { "Decay", 3.5f } }, 0, 0.25f } } },

            // ---- space / creative
            { "Ambient Wash", "Space", {
                { "Chorus", { { "Depth", 0.4f }, { "Voices", 3.0f } }, 0, 0.35f },
                { "Shimmer Reverb", { { "Decay", 6.0f }, { "Shimmer", 0.3f } }, 0, 0.35f },
                { "Stereo Width", { { "Width", 1.25f } } } } },
            { "Dub Delay", "Space", {
                { "Echo", { { "Feedback", 0.55f }, { "Tone", 2200.0f }, { "Drive", 0.4f } }, 0, 0.3f },
                { "Spring Reverb", { { "Decay", 2.0f } }, 0, 0.15f } } },
            { "Harmonic Pad", "Creative", {
                { "Harmonic Resonator", { { "Chord", chordIdx ("maj9") }, { "Root", 0.0f }, { "Low", 80.0f }, { "High", 8000.0f },
                                          { "Reso", 0.75f }, { "Spread", 10.0f } }, 1, 0.6f },
                { "Algorithmic Verb", { { "Decay", 4.0f }, { "Size", 0.8f } }, 0, 0.3f } } },
            { "Harmonic Arp", "Creative", {
                { "Harmonic Resonator", { { "Chord", chordIdx ("m7") }, { "Root", 9.0f }, { "Low", 100.0f }, { "High", 6000.0f },
                                          { "Reso", 0.8f }, { "Arp", 1.0f }, { "Rate", 6.0f }, { "Gate", 0.6f }, { "Out", 9.0f } }, 1, 0.7f },
                { "Ping-Pong Delay", { { "Feedback", 0.35f } }, 0, 0.2f } } },
            { "Lo-Fi Tape", "Creative", {
                { "LoFi Degrader", { { "Wow", 0.3f }, { "Flutter", 0.2f }, { "Crackle", 0.25f }, { "Tone", 7000.0f } } },
                { "Vintage Filter", { { "Cutoff", 6000.0f }, { "Reso", 0.15f }, { "Out", 3.0f } } },
                { "Echo", { { "Feedback", 0.3f }, { "Wow", 0.4f } }, 0, 0.15f } } },
            { "Telephone", "Creative", {
                { "Multimode Filter", { { "Cutoff", 1600.0f }, { "Reso", 0.35f }, { "Out", 5.0f } }, 2 },
                { "Saturation", { { "Drive", 6.0f } }, 2 } } },
            { "Glitch Box", "Creative", {
                { "Stutter", { { "Chance", 0.3f } } },
                { "Glitch", { { "Density", 0.25f } } } } },
            { "Trance Pump", "Creative", {
                { "Ducker", { { "Depth", 0.7f } } } } },
            { "Granular Haze", "Creative", {
                { "Granular Delay", { { "Spray", 0.5f }, { "Pitch", 12.0f }, { "Feedback", 0.4f } }, 0, 0.3f },
                { "Plate Reverb", { { "Decay", 3.5f } }, 0, 0.25f } } },

            // ---- repair
            { "Cleanup: Hum & Clicks", "Repair", {
                { "Hum Remover", {} },
                { "Click Remover", { { "Sens", 0.4f } } } } },
            { "Cleanup: Noise", "Repair", {
                { "Denoiser", { { "Reduce", 8.0f } } } } },
        };
        return f;
    }

    juce::File customUserDir;
    bool useCustomUserDir = false;
}

int numFactory() { return (int) factory().size(); }
juce::String factoryName (int i) { return juce::isPositiveAndBelow (i, numFactory()) ? factory()[(size_t) i].name : ""; }
juce::String factoryTag (int i) { return juce::isPositiveAndBelow (i, numFactory()) ? factory()[(size_t) i].tag : ""; }

bool applyFactory (EfxGridProcessor& proc, int i)
{
    if (! juce::isPositiveAndBelow (i, numFactory())) return false;
    const auto& f = factory()[(size_t) i];
    std::vector<std::pair<int, std::vector<std::pair<juce::String, float>>>> chain;
    std::vector<int> modes;
    std::vector<float> mixes;
    for (const auto& fx : f.chain)
    {
        const int id = efx::idForName (fx.name);
        jassert (id > 0);   // a factory preset names an effect that doesn't exist
        if (id <= 0) continue;
        chain.push_back ({ id, fx.set });
        modes.push_back (fx.mode);
        mixes.push_back (fx.mix);
    }
    proc.applyChain (chain, modes, mixes);
    proc.presetName = f.name;
    proc.presetIndex = i;
    return true;
}

void setUserDirForTest (const juce::File& dir) { customUserDir = dir; useCustomUserDir = true; }

juce::File userDir()
{
    if (useCustomUserDir) return customUserDir;
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Pastel EFX Grid Presets");
}

std::vector<Entry> scanUser()
{
    std::vector<Entry> out;
    const auto dir = userDir();
    if (! dir.isDirectory()) return out;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
        out.push_back ({ true, -1, f.getFileNameWithoutExtension(), "User", f });
    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    for (size_t i = 0; i < out.size(); ++i) out[i].index = (int) i;
    return out;
}

juce::String sanitiseName (juce::String name)
{
    name = name.trim().substring (0, 64);
    name = name.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-()&+.,'");
    name = name.trim();
    return name.isEmpty() ? juce::String ("Untitled") : name;
}

bool saveUser (const juce::String& name, EfxGridProcessor& proc)
{
    const auto clean = sanitiseName (name);
    const auto dir = userDir();
    if (! dir.isDirectory() && ! dir.createDirectory()) return false;
    const auto file = dir.getChildFile (clean + ".xml");
    const auto old = proc.presetName;
    proc.presetName = clean;
    auto xml = proc.presetXml();
    if (xml == nullptr || ! xml->writeTo (file)) { proc.presetName = old; return false; }
    proc.presetIndex = numFactory();
    const auto users = scanUser();
    for (const auto& u : users) if (u.file == file) proc.presetIndex = numFactory() + u.index;
    return true;
}

bool loadUser (const juce::File& file, EfxGridProcessor& proc)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! proc.loadPresetXml (*xml)) return false;   // nothing changed on failure
    proc.presetName = file.getFileNameWithoutExtension();
    return true;
}

bool deleteUser (const juce::File& file)
{
    if (file.getParentDirectory() != userDir()) return false;   // safety: own folder only
    return file.existsAsFile() && file.deleteFile();
}

std::vector<Entry> allEntries()
{
    std::vector<Entry> all;
    for (int i = 0; i < numFactory(); ++i) all.push_back ({ false, i, factoryName (i), factoryTag (i), {} });
    for (auto& u : scanUser()) all.push_back (u);
    return all;
}

bool loadEntry (const Entry& e, EfxGridProcessor& proc)
{
    if (! e.isUser) return applyFactory (proc, e.index);
    if (! loadUser (e.file, proc)) return false;
    proc.presetIndex = numFactory() + e.index;
    return true;
}

bool step (EfxGridProcessor& proc, int delta)
{
    const auto all = allEntries();
    if (all.empty()) return false;
    int cur = proc.presetIndex;
    if (cur < 0)   // edited / unknown: find it by name, else start from the top
        for (size_t i = 0; i < all.size(); ++i) if (all[i].name == proc.presetName) cur = (int) i;
    const int n = (int) all.size();
    const int next = cur < 0 ? (delta > 0 ? 0 : n - 1) : ((cur + delta) % n + n) % n;
    return loadEntry (all[(size_t) next], proc);
}
}
