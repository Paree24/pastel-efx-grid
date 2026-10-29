#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "dsp/Effects.h"
#include "MidiLearn.h"
#include "Sequencer.h"

// ============================================================
// 16 slots in series. Each slot owns one GridEffect, rebuilt on the message
// thread whenever its effect changes (old instances retire, then get purged).
//
// Per slot the APVTS holds the effect, mode, 8 controls, macro, mix and bypass.
// Two pieces of slot state are not automatable and live in the state tree
// instead: the user's macro routing (a depth per control) and the slot's
// wavetable (for the wavetable effects).
// ============================================================
class EfxGridProcessor : public juce::AudioProcessor,
                         private juce::AudioProcessorValueTreeState::Listener,
                         private juce::AsyncUpdater,
                         private juce::Timer
{
public:
    static constexpr int kSlots = 16;
    static constexpr int kMaxParams = efx::kMaxParams;
    static constexpr int kStateVersion = 5;   // 4: ADSR (#19) became the Harmonic Resonator; 5: effect sequencer
    static constexpr int kDataSize = efx::kDataSize;

    EfxGridProcessor();
    ~EfxGridProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Pastel EFX Grid"; }
    bool acceptsMidi() const override { return true; }   // only for MIDI learn: notes are ignored
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- effect sequencer (message thread edits; the audio thread reads atomics)
    // Lanes live with their slot (SlotState::lanes), so moving, removing and
    // replacing effects, undo and presets carry them along.
    seq::SlotLanes getSlotLanes (int slot) const;
    void setSlotLanes (int slot, const seq::SlotLanes&);
    uint32_t laneSteps (int slot, int scene) const;
    void setLaneSteps (int slot, int scene, uint32_t mask);
    void setLaneMode (int slot, int mode);
    void setLaneTail (int slot, int tail);
    void setLaneTiming (int slot, int scene, int len, int rate);   // 0 / -1 = the scene's
    seq::SceneSettings sceneSettings (int scene) const;
    void setSceneSettings (int scene, const seq::SceneSettings&);
    bool seqEnabled() const;
    void setSeqEnabled (bool on);
    int playingScene() const { return seqPlaying.load(); }
    int queuedScene() const { return seqQueued.load(); }
    void requestScene (int scene);             // switches at the end of the pattern while playing
    bool slotHasTail (int slot) const;         // Ring is offered only for delays / reverbs
    bool slotGateOpen (int slot) const { return gateUi[(size_t) juce::jlimit (0, kSlots - 1, slot)].load(); }
    double seqPpq() const { return seqPpqUi.load(); }   // playhead (beats)
    bool seqRunning() const { return seqRunningUi.load(); }
    void seqEdited();                          // dirty flag + state generation after a sequencer edit
    void setLivePad (int slot, bool down) { if (slot >= 0 && slot < kSlots) livePadUi[(size_t) slot].store (down); }
    // settings: MIDI note maps, gate fade, when a requested scene switches
    enum SceneSwitch { SwitchPattern = 0, SwitchBeat = 1, SwitchNow = 2 };
    int sceneNoteBase() const { return sceneNoteA.load(); }
    int padNoteBase() const { return padNoteA.load(); }
    int sceneSwitchMode() const { return switchModeA.load(); }
    float gateFadeMs() const { return seqFadeMs.load(); }
    void setSceneNoteBase (int note) { sceneNoteA.store (juce::jlimit (0, 127 - seq::kScenes, note)); seqEdited(); }
    void setPadNoteBase (int note) { padNoteA.store (juce::jlimit (0, 127 - kSlots, note)); seqEdited(); }
    void setSceneSwitchMode (int m) { switchModeA.store (juce::jlimit (0, 2, m)); seqEdited(); }
    void setGateFadeMs (float ms) { seqFadeMs.store (juce::jlimit (1.0f, 10.0f, ms)); seqEdited(); }
    // dice and mutate (one undo step each); steps favour beats over off-beats
    void diceLane (int slot, int scene);
    void diceScene (int scene);
    void mutateScene (int scene, float amount);
    void clearScene (int scene);
    void copyScene (int from, int to);
    bool sceneHasSteps (int scene) const;

    // ---- undo / redo (message thread) ------------------------------------
    // Snapshots of every slot (knobs, mode, mix, routing, shape data, tables /
    // IRs by pointer). The editor marks the baseline when a gesture starts and
    // checkpoints when it ends; slot operations and file loads checkpoint
    // themselves. A checkpoint pushes the baseline if anything changed.
    void undoBaseline();                      // adopt the current state without recording
    void undoCheckpoint (bool mergeRecent = false);   // merge: fold into the last step if it's < 0.8 s old
    bool undoStep();
    bool redoStep();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    void clearUndo();
    // a batch of edits (preset load, randomize) becomes one undo step
    struct UndoBatch
    {
        EfxGridProcessor& p;
        explicit UndoBatch (EfxGridProcessor& pr) : p (pr) { p.undoCheckpoint(); ++p.undoHold; }
        ~UndoBatch() { --p.undoHold; p.undoCheckpoint(); }
    };

    // ---- presets (message thread) ----------------------------------------
    juce::String presetName { "Init" };
    int presetIndex = -1;                     // position in the browser's list (-1 = edited / unknown)
    bool loadPresetXml (const juce::XmlElement&);   // one undo step
    std::unique_ptr<juce::XmlElement> presetXml();  // the full state, for saving
    void applyChain (const std::vector<std::pair<int, std::vector<std::pair<juce::String, float>>>>& chain,
                     const std::vector<int>& modes, const std::vector<float>& mixes);   // factory chains

    // ---- randomize / mutate (one undo step each) ------------------------------
    void randomizeSlot (int slot, float intensity);
    void mutateSlot (int slot, float amount);
    void randomizeChain (float intensity);
    void mutateChain (float amount);
    void randomNewChain();                    // a fresh chain of 3-5 random effects
    juce::AudioProcessorValueTreeState apvts;
    pastel::MidiLearn midiLearn { *this, "Pastel EFX Grid" };   // global CC map + preset knob

    // ---- editor helpers --------------------------------------------------
    static juce::String paramId (int slot, const juce::String& suffix);
    efx::GridEffect* getEffect (int slot) const { return slots[(size_t) juce::jlimit (0, kSlots - 1, slot)].active.load (std::memory_order_acquire); }
    int getSlotEffectId (int slot) const;
    int getSlotMode (int slot) const;
    float getNorm (int slot, int k) const;
    bool sidechainAvailable() const { return scActive.load(); }

    // ---- slot editing (message thread) -------------------------------------
    // The grid is an ordered chain packed into slots 1..N. Every operation
    // moves whole slot states, so settings travel with their effect.
    void loadEffect (int slot, int fxId);      // fxId 0 clears; applies the effect's defaults
    bool appendEffect (int fxId);              // load at the first empty slot
    void clearSlotCompact (int slot);          // remove + shift later slots up
    void moveSlot (int from, int to);          // move to a position, shifting the others
    void resetSlot (int slot);                 // back to the effect's defaults
    void setSlotMode (int slot, int modeIndex);
    int  firstEmptySlot() const;
    int  chainLength() const;
    void reloadSlotsNow();                     // rebuild every slot's DSP now (message thread)

    // macro routing: depth -1..1 per control (normalised change at macro 100%)
    float getMacroDepth (int slot, int k) const;
    void setMacroDepth (int slot, int k, float depth);
    void resetMacroRouting (int slot);          // the effect's default routing
    juce::String macroLabel (int slot) const;   // short description of what the macro drives

    // shape data (drawn curves, steps, EQ bands ...): edited by the visual editors
    float getSlotData (int slot, int i) const;
    void getSlotDataAll (int slot, float* out) const;
    void setSlotData (int slot, int i, float v);
    void setSlotDataBlock (int slot, int start, const float* v, int n);
    void resetSlotData (int slot);
    int  slotDataGeneration (int slot) const { return dataGen[juce::jlimit (0, kSlots - 1, slot)].load(); }

    // user audio (Convolution Verb impulse response)
    bool loadAudioAsset (int slot, const juce::File& file, juce::String& error);
    void clearAudioAsset (int slot);
    std::shared_ptr<const efx::AudioAsset> getAudioAsset (int slot) const { return assets[(size_t) juce::jlimit (0, kSlots - 1, slot)]; }

    // wavetables (wavetable filter / drive)
    bool loadWavetable (int slot, const juce::File& file, juce::String& error);
    void resetWavetable (int slot);
    std::shared_ptr<const efx::Wavetable> getWavetable (int slot) const;

    std::function<void()> onSlotsChanged;       // editor refresh hook (message thread)
    int stateGeneration() const { return stateGen.load(); }   // bumps when a whole state is loaded

private:
    struct Slot
    {
        std::unique_ptr<efx::GridEffect> owned;
        std::atomic<efx::GridEffect*> active { nullptr };
    };
    struct Retired { std::unique_ptr<efx::GridEffect> fx; juce::uint32 when; };
    struct RetiredTable { std::shared_ptr<const efx::Wavetable> t; juce::uint32 when; };
    struct RetiredAsset { std::shared_ptr<const efx::AudioAsset> a; juce::uint32 when; };

    struct SlotState
    {
        int fx = 0, mode = 0;
        float p[kMaxParams] {};
        float macro = 0.0f, mix = 1.0f;
        bool bypass = false;
        float depth[kMaxParams] {};
        float data[efx::kDataSize] {};
        std::shared_ptr<const efx::Wavetable> table;
        std::shared_ptr<const efx::AudioAsset> asset;
        seq::SlotLanes lanes;   // the effect sequencer's lanes travel with the slot
    };
    struct Snapshot
    {
        std::array<SlotState, kSlots> slots;
        seq::SceneSettings scenes[seq::kScenes];
        SlotState& operator[] (size_t i) { return slots[i]; }
        const SlotState& operator[] (size_t i) const { return slots[i]; }
    };
    Snapshot takeSnapshot() const;
    void applySnapshot (const Snapshot&);
    static bool sameSlot (const SlotState&, const SlotState&);
    std::vector<Snapshot> undoStack, redoStack;
    int undoHold = 0;
    juce::Random rng;
    bool applyStateXml (const juce::XmlElement&);
    void randomizeSlotNoUndo (int slot, float intensity);
    void mutateSlotNoUndo (int slot, float amount);
    std::unique_ptr<Snapshot> undoBase;
    juce::uint32 lastPushMs = 0;
    static constexpr size_t kUndoDepth = 64;
    SlotState getSlotState (int slot) const;
    void setSlotState (int slot, const SlotState&);
    SlotState defaultState (int fxId) const;
    void setParamNorm (const juce::String& id, float norm);
    void setTable (int slot, std::shared_ptr<const efx::Wavetable> t);
    void setAsset (int slot, std::shared_ptr<const efx::AudioAsset> a);
    void migrateV3Slots();   // v3 -> v4: ADSR slots to Transient Shaper, shaper Decay default
    void migrateV2Slot (int slot, const float* oldNorm, int oldMode, const float* oldDepth);

    void parameterChanged (const juce::String& id, float newValue) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void buildSlot (int slot);
    void rebuildAll();
    void writeExtrasToState();
    void readExtrasFromState();

    std::array<Slot, kSlots> slots;
    std::vector<Retired> retired;
    std::vector<RetiredTable> retiredTables;
    std::array<std::shared_ptr<const efx::Wavetable>, kSlots> tables;
    std::array<std::shared_ptr<const efx::AudioAsset>, kSlots> assets;
    std::vector<RetiredAsset> retiredAssets;
    std::atomic<float> slotData[kSlots][efx::kDataSize] {};
    std::atomic<int> dataGen[kSlots] {};
    std::atomic<bool> pending[kSlots] {};
    std::atomic<bool> anyPending { false };
    std::atomic<bool> scActive { false };
    std::atomic<float> macroDepth[kSlots][kMaxParams] {};

    // effect sequencer storage (lanes per slot and scene, scene timing)
    std::atomic<uint32_t> laneStepsA[kSlots][seq::kScenes] {};
    std::atomic<int> laneLenA[kSlots][seq::kScenes] {}, laneRateA[kSlots][seq::kScenes] {};
    std::atomic<int> laneModeA[kSlots] {}, laneTailA[kSlots] {};
    std::atomic<int> sceneStepsA[seq::kScenes] {}, sceneRateA[seq::kScenes] {};
    std::atomic<float> sceneSwingA[seq::kScenes] {};
    std::atomic<float> seqFadeMs { 3.0f };
    std::atomic<int> sceneNoteA { 36 }, padNoteA { 60 }, switchModeA { 0 };
    std::atomic<int> seqPlaying { 0 }, seqQueued { -1 };
    std::array<std::atomic<bool>, kSlots> gateUi {}, livePadUi {};
    std::atomic<double> seqPpqUi { 0.0 };
    std::atomic<bool> seqRunningUi { false };
    juce::AudioParameterBool* seqOnParam = nullptr;
    juce::AudioParameterChoice* seqSceneParam = nullptr;
    // audio thread only
    bool gateState[kSlots] {};
    const efx::GridEffect* gateFx[kSlots] {};
    int lastSceneParam = -1;
    bool livePad[kSlots] {};
    efx::SlotInput::GateEvent gateEv[kSlots][seq::kMaxEvents] {};
    int gateEvN[kSlots] {};
    void scheduleSequencer (double ppq, double beatsPerSample, int n, bool hostPlaying, const juce::MidiBuffer& midi);
    double currentSampleRate = 44100.0;
    int currentBlock = 512;
    double freeRunPpq = 0.0;

    juce::AudioParameterChoice* fxChoice[kSlots] {};
    juce::AudioParameterChoice* modeChoice[kSlots] {};
    juce::AudioParameterBool*   bypassParam[kSlots] {};
    std::atomic<float>* knobRaw[kSlots][kMaxParams] {};
    std::atomic<float>* macroRaw[kSlots] {};
    std::atomic<float>* mixRaw[kSlots] {};

    juce::AudioBuffer<float> scScratch;
    std::atomic<int> stateGen { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EfxGridProcessor)
};
