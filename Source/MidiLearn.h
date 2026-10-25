#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Global MIDI learn, shared by every instance of the plugin in every DAW
// session: the CC -> parameter map lives in
// ~/.config/PastelMidiLearn/<plugin>.xml, never in the project or preset.
// One extra CC can be the "preset knob" that steps through presets.
//
// Learning: switch learn on, touch a control on screen (any parameter
// gesture arms it), then move a hardware knob. The first few CC values decide
// how the knob talks: absolute 0..127, or a relative encoder (Arturia style
// 64 +/- n, or two's complement 1..63 up / 65..127 down), so endless knobs
// turn the right way.
namespace pastel
{
class MidiLearn : private juce::AudioProcessorParameter::Listener,
                  private juce::Timer
{
public:
    enum class Enc { absolute, offset64, twos };

    MidiLearn(juce::AudioProcessor& processor, const juce::String& pluginName);
    ~MidiLearn() override;

    // audio thread: apply mapped CCs, feed learning, queue preset steps
    void process(const juce::MidiBuffer& midi);

    // message thread: +1 next preset, -1 previous (called from a timer)
    std::function<void(int delta)> onPresetStep;

    void setLearning(bool on);
    bool isLearning() const { return learning.load(); }
    void armPresetKnob();               // the next knob moved becomes the preset knob
    juce::String getArmedName() const;  // "" when nothing is armed
    juce::String getLastLearned() const { return lastLearned; }
    int getRevision() const { return revision.load(); } // bumps on every learn-state / mapping change

    struct Mapping { int cc; juce::String paramId, name; Enc enc; };
    std::vector<Mapping> getMappings() const;
    int getPresetKnobCC() const { return presetCC.load(); }
    void clearCC(int cc);
    void clearPresetKnob();
    void clearAll();

    // message-thread housekeeping (finish a learn, preset steps, reload
    // mappings saved by other instances); the timer calls it, tests may too
    void tick();

    juce::File getFile() const { return file; }
    static juce::File fileFor(const juce::String& pluginName);
    static void setDirectoryForTest(const juce::File& dir); // harness override, else ~/.config
    static Enc detectEncoding(const std::vector<int>& values);
    static int relativeDelta(Enc enc, int value);

private:
    static constexpr int kPresetArmed = -2;
    static constexpr int kLearnSamples = 4;

    void parameterValueChanged(int, float) override {}
    void parameterGestureChanged(int index, bool starting) override;
    void timerCallback() override { tick(); }
    void finishLearn();
    void load();
    void save();
    void rebuildIndex();
    int indexOf(const juce::String& paramId) const;
    void changed() { ++revision; }

    juce::AudioProcessor& proc;
    juce::Array<juce::AudioProcessorParameter*> params;
    juce::File file;
    juce::Time fileTime;

    struct Entry { juce::String paramId; Enc enc = Enc::absolute; };
    std::map<int, Entry> ccMap;                   // message thread copy, persisted
    std::array<std::atomic<int>, 128> ccIndex;    // audio thread: param index or -1
    std::array<std::atomic<int>, 128> ccEnc;      // audio thread: Enc per CC
    std::atomic<int> presetCC { -1 };
    std::atomic<int> presetEnc { (int)Enc::absolute };
    int lastPresetAbs = -1;                       // audio thread only

    std::atomic<bool> learning { false };
    std::atomic<int> armed { -1 };                // param index, kPresetArmed or -1
    // learn input from the audio thread: first CC heard + its first values
    std::atomic<int> heardCC { -1 }, heardCount { 0 };
    std::array<std::atomic<int>, kLearnSamples> heardVals;
    int heardTicks = 0;
    std::atomic<int> pendingSteps { 0 };
    int fileTicks = 0;
    std::atomic<int> revision { 0 };
    juce::String lastLearned;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiLearn)
};

// Header button: click for the MIDI menu (learn on/off, preset knob,
// mappings). Shows short hints in a bubble while learning.
class MidiLearnButton : public juce::TextButton, private juce::Timer
{
public:
    explicit MidiLearnButton(MidiLearn& ml);
    ~MidiLearnButton() override;

private:
    void clicked() override;
    void timerCallback() override;
    void hint(const juce::String& message, int ms);

    MidiLearn& learn;
    juce::BubbleMessageComponent bubble;
    int seenRevision = -1;
    juce::String seenArmed;
};
} // namespace pastel
