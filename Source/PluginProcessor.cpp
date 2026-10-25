#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"

juce::String EfxGridProcessor::paramId (int slot, const juce::String& suffix)
{
    return "s" + juce::String (slot) + "_" + suffix;
}

EfxGridProcessor::EfxGridProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                            .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "PARAMS", [this]
      {
          juce::AudioProcessorValueTreeState::ParameterLayout layout;
          juce::StringArray fxNames;
          fxNames.add ("Empty");
          for (int i = 0; i < efx::kNumEffects; ++i) fxNames.add (efx::effectDef (i).name);
          const juce::StringArray modeNames { "1", "2", "3", "4" };
          for (int s = 0; s < kSlots; ++s)
          {
              const auto id = [s] (const juce::String& suf) { return juce::ParameterID { paramId (s, suf), 1 }; };
              const auto pre = "S" + juce::String (s + 1).paddedLeft ('0', 2) + " ";
              layout.add (std::make_unique<juce::AudioParameterChoice> (id ("fx"), pre + "Effect", fxNames, 0));
              layout.add (std::make_unique<juce::AudioParameterChoice> (id ("mode"), pre + "Mode", modeNames, 0,
                  juce::AudioParameterChoiceAttributes().withStringFromValueFunction ([this, s] (int idx, int)
                  {
                      const auto* d = efx::defForId (getSlotEffectId (s));
                      return (d != nullptr && idx < d->numModes) ? juce::String (d->modes[idx]) : juce::String ("-");
                  })));
              for (int k = 0; k < kMaxParams; ++k)
                  layout.add (std::make_unique<juce::AudioParameterFloat> (id ("p" + juce::String (k)), pre + "P" + juce::String (k + 1),
                      juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f,
                      juce::AudioParameterFloatAttributes().withStringFromValueFunction ([this, s, k] (float v, int)
                      {
                          const auto* d = efx::defForId (getSlotEffectId (s));
                          return (d != nullptr && k < d->numParams) ? juce::String (d->p[k].name) + " " + efx::formatParam (d->p[k], v)
                                                                    : juce::String ("-");
                      })));
              layout.add (std::make_unique<juce::AudioParameterFloat> (id ("macro"), pre + "Macro", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
                  juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; })));
              layout.add (std::make_unique<juce::AudioParameterFloat> (id ("mix"), pre + "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f,
                  juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; })));
              layout.add (std::make_unique<juce::AudioParameterBool> (id ("bypass"), pre + "Bypass", false));
          }
          return layout;
      }())
{
    for (int s = 0; s < kSlots; ++s)
    {
        const auto p = [s] (const juce::String& suf) { return paramId (s, suf); };
        fxChoice[s] = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (p ("fx")));
        modeChoice[s] = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (p ("mode")));
        bypassParam[s] = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (p ("bypass")));
        for (int k = 0; k < kMaxParams; ++k) knobRaw[s][k] = apvts.getRawParameterValue (p ("p" + juce::String (k)));
        macroRaw[s] = apvts.getRawParameterValue (p ("macro"));
        mixRaw[s] = apvts.getRawParameterValue (p ("mix"));
        apvts.addParameterListener (p ("fx"), this);
        for (int k = 0; k < kMaxParams; ++k) macroDepth[s][k].store (0.0f);
    }
    startTimerHz (10);   // retired-instance purge + effect idle work
    setLatencySamples (0);
    midiLearn.onPresetStep = [this] (int delta) { presets::step (*this, delta); };
}

EfxGridProcessor::~EfxGridProcessor()
{
    for (int s = 0; s < kSlots; ++s) apvts.removeParameterListener (paramId (s, "fx"), this);
}

void EfxGridProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    currentBlock = juce::jmax (1, samplesPerBlock);
    scScratch.setSize (2, currentBlock);
    rebuildAll();
}

void EfxGridProcessor::releaseResources()
{
    for (auto& s : slots) { s.active.store (nullptr); s.owned.reset(); }
    retired.clear();
}

bool EfxGridProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    const auto in = layouts.getMainInputChannelSet();
    if (in != juce::AudioChannelSet::stereo() && in != juce::AudioChannelSet::mono()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

// ------------------------------------------------------------------ slot model
int EfxGridProcessor::getSlotEffectId (int slot) const
{
    slot = juce::jlimit (0, kSlots - 1, slot);
    return fxChoice[slot] != nullptr ? fxChoice[slot]->getIndex() : 0;
}

int EfxGridProcessor::getSlotMode (int slot) const
{
    slot = juce::jlimit (0, kSlots - 1, slot);
    const auto* d = efx::defForId (getSlotEffectId (slot));
    const int nm = d != nullptr ? juce::jmax (1, d->numModes) : 1;
    return juce::jlimit (0, nm - 1, modeChoice[slot] != nullptr ? modeChoice[slot]->getIndex() : 0);
}

float EfxGridProcessor::getNorm (int slot, int k) const
{
    slot = juce::jlimit (0, kSlots - 1, slot);
    k = juce::jlimit (0, kMaxParams - 1, k);
    return knobRaw[slot][k] != nullptr ? knobRaw[slot][k]->load() : 0.0f;
}

void EfxGridProcessor::setParamNorm (const juce::String& id, float norm)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, norm));
        p->endChangeGesture();
    }
}

EfxGridProcessor::SlotState EfxGridProcessor::getSlotState (int slot) const
{
    SlotState st;
    st.fx = getSlotEffectId (slot);
    st.mode = modeChoice[slot] != nullptr ? modeChoice[slot]->getIndex() : 0;
    for (int k = 0; k < kMaxParams; ++k) { st.p[k] = getNorm (slot, k); st.depth[k] = macroDepth[slot][k].load(); }
    st.macro = macroRaw[slot] != nullptr ? macroRaw[slot]->load() : 0.0f;
    st.mix = mixRaw[slot] != nullptr ? mixRaw[slot]->load() : 1.0f;
    st.bypass = bypassParam[slot] != nullptr && bypassParam[slot]->get();
    st.table = tables[(size_t) slot];
    st.asset = assets[(size_t) slot];
    for (int i = 0; i < efx::kDataSize; ++i) st.data[i] = slotData[slot][i].load();
    return st;
}

EfxGridProcessor::SlotState EfxGridProcessor::defaultState (int fxId) const
{
    SlotState st;
    st.fx = fxId;
    if (const auto* d = efx::defForId (fxId))
    {
        for (int k = 0; k < kMaxParams; ++k) { st.p[k] = d->p[k].def; st.depth[k] = d->macroDepth[k]; }
        st.mix = d->mixDef;
        if (auto fx = efx::createEffect (fxId)) fx->defaultData (st.data);   // the effect defines its shape-data layout
    }
    return st;
}

void EfxGridProcessor::setSlotState (int slot, const SlotState& st)
{
    if (slot < 0 || slot >= kSlots) return;
    // knobs / mix / routing first so a rebuilt effect starts from the right values
    for (int k = 0; k < kMaxParams; ++k)
    {
        setParamNorm (paramId (slot, "p" + juce::String (k)), st.p[k]);
        macroDepth[slot][k].store (st.depth[k]);
    }
    setParamNorm (paramId (slot, "macro"), st.macro);
    setParamNorm (paramId (slot, "mix"), st.mix);
    if (modeChoice[slot] != nullptr) setParamNorm (paramId (slot, "mode"), modeChoice[slot]->convertTo0to1 ((float) st.mode));
    if (bypassParam[slot] != nullptr) setParamNorm (paramId (slot, "bypass"), st.bypass ? 1.0f : 0.0f);
    setTable (slot, st.table);
    setAsset (slot, st.asset);
    for (int i = 0; i < efx::kDataSize; ++i) slotData[slot][i].store (st.data[i]);
    ++dataGen[slot];
    if (fxChoice[slot] != nullptr) setParamNorm (paramId (slot, "fx"), fxChoice[slot]->convertTo0to1 ((float) st.fx));
}

void EfxGridProcessor::loadEffect (int slot, int fxId)
{
    if (slot < 0 || slot >= kSlots) return;
    if (fxId <= 0) { clearSlotCompact (slot); return; }
    setSlotState (slot, defaultState (fxId));
    undoCheckpoint();
    if (onSlotsChanged) onSlotsChanged();
}

void EfxGridProcessor::resetSlot (int slot)
{
    const int fx = getSlotEffectId (slot);
    if (fx > 0) { auto st = defaultState (fx); st.table = tables[(size_t) slot]; st.asset = assets[(size_t) slot]; setSlotState (slot, st); }
    undoCheckpoint();
    if (onSlotsChanged) onSlotsChanged();
}

void EfxGridProcessor::setSlotMode (int slot, int modeIndex)
{
    if (slot < 0 || slot >= kSlots || modeChoice[slot] == nullptr) return;
    setParamNorm (paramId (slot, "mode"), modeChoice[slot]->convertTo0to1 ((float) modeIndex));
}

int EfxGridProcessor::firstEmptySlot() const
{
    for (int s = 0; s < kSlots; ++s) if (getSlotEffectId (s) == 0) return s;
    return -1;
}

int EfxGridProcessor::chainLength() const
{
    int n = 0;
    for (int s = 0; s < kSlots; ++s) if (getSlotEffectId (s) != 0) ++n;
    return n;
}

bool EfxGridProcessor::appendEffect (int fxId)
{
    if (fxId < 1 || fxId > efx::kNumEffects) return false;
    const int slot = firstEmptySlot();
    if (slot < 0) return false;
    loadEffect (slot, fxId);
    return true;
}

void EfxGridProcessor::clearSlotCompact (int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    std::vector<SlotState> states;
    for (int s = slot + 1; s < kSlots; ++s) states.push_back (getSlotState (s));
    for (int s = slot; s < kSlots - 1; ++s) setSlotState (s, states[(size_t) (s - slot)]);
    setSlotState (kSlots - 1, defaultState (0));
    undoCheckpoint();
    if (onSlotsChanged) onSlotsChanged();
}

void EfxGridProcessor::moveSlot (int from, int to)
{
    const int last = juce::jmax (0, chainLength() - 1);
    from = juce::jlimit (0, kSlots - 1, from);
    to = juce::jlimit (0, last, to);
    if (from == to || getSlotEffectId (from) == 0) return;
    std::vector<SlotState> st;
    for (int s = 0; s < kSlots; ++s) st.push_back (getSlotState (s));
    auto moved = st[(size_t) from];
    st.erase (st.begin() + from);
    st.insert (st.begin() + to, moved);
    for (int s = juce::jmin (from, to); s <= juce::jmax (from, to); ++s) setSlotState (s, st[(size_t) s]);
    undoCheckpoint();
    if (onSlotsChanged) onSlotsChanged();
}

void EfxGridProcessor::reloadSlotsNow()
{
    for (auto& p : pending) p.store (false);
    rebuildAll();
}

// ------------------------------------------------------------------ macro routing
float EfxGridProcessor::getMacroDepth (int slot, int k) const
{
    return macroDepth[juce::jlimit (0, kSlots - 1, slot)][juce::jlimit (0, kMaxParams - 1, k)].load();
}

void EfxGridProcessor::setMacroDepth (int slot, int k, float depth)
{
    if (slot < 0 || slot >= kSlots || k < 0 || k >= kMaxParams) return;
    macroDepth[slot][k].store (juce::jlimit (-1.0f, 1.0f, depth));
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void EfxGridProcessor::resetMacroRouting (int slot)
{
    const auto st = defaultState (getSlotEffectId (slot));
    for (int k = 0; k < kMaxParams; ++k) macroDepth[slot][k].store (st.depth[k]);
    undoCheckpoint();
}

juce::String EfxGridProcessor::macroLabel (int slot) const
{
    const auto* d = efx::defForId (getSlotEffectId (slot));
    if (d == nullptr || d->cat == efx::CatAnalysis) return "-";
    juce::StringArray names;
    bool isDefault = true;
    for (int k = 0; k < d->numParams; ++k)
    {
        const float dep = macroDepth[slot][k].load();
        if (std::abs (dep - d->macroDepth[k]) > 0.005f) isDefault = false;
        if (std::abs (dep) > 0.005f) names.add (d->p[k].name);
    }
    if (names.isEmpty()) return "Unassigned";
    if (isDefault) return d->macroName;
    if (names.size() == 1) return names[0];
    return names[0] + " +" + juce::String (names.size() - 1);
}

// ------------------------------------------------------------------ shape data
float EfxGridProcessor::getSlotData (int slot, int i) const
{
    return slotData[juce::jlimit (0, kSlots - 1, slot)][juce::jlimit (0, efx::kDataSize - 1, i)].load();
}

void EfxGridProcessor::getSlotDataAll (int slot, float* out) const
{
    slot = juce::jlimit (0, kSlots - 1, slot);
    for (int i = 0; i < efx::kDataSize; ++i) out[i] = slotData[slot][i].load();
}

void EfxGridProcessor::setSlotData (int slot, int i, float v)
{
    if (slot < 0 || slot >= kSlots || i < 0 || i >= efx::kDataSize) return;
    slotData[slot][i].store (v);
    ++dataGen[slot];
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void EfxGridProcessor::setSlotDataBlock (int slot, int start, const float* v, int n)
{
    if (slot < 0 || slot >= kSlots) return;
    for (int i = 0; i < n && start + i < efx::kDataSize; ++i) slotData[slot][start + i].store (v[i]);
    ++dataGen[slot];
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void EfxGridProcessor::resetSlotData (int slot)
{
    const auto st = defaultState (getSlotEffectId (slot));
    setSlotDataBlock (slot, 0, st.data, efx::kDataSize);
}

// ------------------------------------------------------------------ user audio (IR)
void EfxGridProcessor::setAsset (int slot, std::shared_ptr<const efx::AudioAsset> a)
{
    auto& cur = assets[(size_t) slot];
    if (cur == a) return;
    if (cur != nullptr) retiredAssets.push_back ({ cur, juce::Time::getMillisecondCounter() });
    cur = std::move (a);
    if (auto* fx = slots[(size_t) slot].owned.get()) fx->setAsset (cur.get());
}

bool EfxGridProcessor::loadAudioAsset (int slot, const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "Not a readable audio file."; return false; }
    const int len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 10.0));
    if (len < 16) { error = "The file is too short."; return false; }
    auto a = std::make_shared<efx::AudioAsset>();
    a->buf.setSize (juce::jlimit (1, 2, (int) reader->numChannels), len);
    reader->read (&a->buf, 0, len, 0, true, a->buf.getNumChannels() > 1);
    a->sampleRate = reader->sampleRate;
    a->name = file.getFileNameWithoutExtension();
    setAsset (slot, a);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    undoCheckpoint();
    return true;
}

void EfxGridProcessor::clearAudioAsset (int slot)
{
    setAsset (slot, nullptr);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    undoCheckpoint();
}

// ------------------------------------------------------------------ wavetables
void EfxGridProcessor::setTable (int slot, std::shared_ptr<const efx::Wavetable> t)
{
    auto& cur = tables[(size_t) slot];
    if (cur == t) return;
    if (cur != nullptr) retiredTables.push_back ({ cur, juce::Time::getMillisecondCounter() });
    cur = std::move (t);
    if (auto* fx = slots[(size_t) slot].owned.get())
        if (fx->def().usesWavetable()) fx->setWavetable (cur != nullptr ? cur.get() : efx::Wavetable::builtIn().get());
}

std::shared_ptr<const efx::Wavetable> EfxGridProcessor::getWavetable (int slot) const
{
    const auto& t = tables[(size_t) juce::jlimit (0, kSlots - 1, slot)];
    return t != nullptr ? t : efx::Wavetable::builtIn();
}

void EfxGridProcessor::resetWavetable (int slot)
{
    setTable (slot, nullptr);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    undoCheckpoint();
}

bool EfxGridProcessor::loadWavetable (int slot, const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "Not a readable audio file."; return false; }
    const auto len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) 2048 * 256);
    if (len < 16) { error = "The file is too short."; return false; }
    juce::AudioBuffer<float> buf (1, len);
    reader->read (&buf, 0, len, 0, true, false);
    // multi-frame tables use 2048-sample frames; short files are one cycle
    int frameLen = 0;
    if (len % 2048 == 0) frameLen = 2048;
    else if (len > 4096) frameLen = 2048;   // trailing partial frame is dropped
    auto t = efx::Wavetable::fromSamples (buf.getReadPointer (0), len, frameLen, file.getFileNameWithoutExtension());
    if (t == nullptr || t->numFrames <= 0) { error = "Could not build a wavetable from this file."; return false; }
    setTable (slot, t);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    undoCheckpoint();
    return true;
}

// ------------------------------------------------------------------ DSP instances
void EfxGridProcessor::buildSlot (int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    const int fxId = getSlotEffectId (slot);
    std::unique_ptr<efx::GridEffect> nf;
    if (fxId >= 1)
    {
        nf = efx::createEffect (fxId);
        if (nf != nullptr)
        {
            nf->prepare (currentSampleRate, currentBlock);
            if (nf->def().usesWavetable() && tables[(size_t) slot] != nullptr) nf->setWavetable (tables[(size_t) slot].get());
            nf->setAsset (assets[(size_t) slot].get());
        }
    }
    auto old = std::move (slots[(size_t) slot].owned);
    slots[(size_t) slot].owned = std::move (nf);
    slots[(size_t) slot].active.store (slots[(size_t) slot].owned.get(), std::memory_order_release);
    if (old != nullptr) retired.push_back ({ std::move (old), juce::Time::getMillisecondCounter() });
}

void EfxGridProcessor::rebuildAll()
{
    for (int s = 0; s < kSlots; ++s) buildSlot (s);
}

void EfxGridProcessor::parameterChanged (const juce::String& id, float)
{
    if (! id.endsWith ("_fx")) return;
    const int slot = id.fromFirstOccurrenceOf ("s", false, false).getIntValue();
    if (slot < 0 || slot >= kSlots) return;
    pending[slot].store (true);
    anyPending.store (true);
    triggerAsyncUpdate();
}

void EfxGridProcessor::handleAsyncUpdate()
{
    if (! anyPending.exchange (false)) return;
    for (int s = 0; s < kSlots; ++s)
        if (pending[s].exchange (false)) buildSlot (s);
}

void EfxGridProcessor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    retired.erase (std::remove_if (retired.begin(), retired.end(),
                   [now] (const Retired& r) { return now - r.when > 1000; }), retired.end());
    retiredTables.erase (std::remove_if (retiredTables.begin(), retiredTables.end(),
                         [now] (const RetiredTable& r) { return now - r.when > 2000; }), retiredTables.end());
    retiredAssets.erase (std::remove_if (retiredAssets.begin(), retiredAssets.end(),
                         [now] (const RetiredAsset& r) { return now - r.when > 2000; }), retiredAssets.end());
    for (auto& s : slots)
        if (auto* fx = s.owned.get()) fx->idle();
}

// ------------------------------------------------------------------ audio
void EfxGridProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midiLearn.process (midi);   // learned CCs land before the chain runs
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 2) return;

    // sidechain first: with a mono main input its channels can alias output 1
    const float* scL = nullptr;
    const float* scR = nullptr;
    if (getBusCount (true) > 1)
        if (auto* bus = getBus (true, 1); bus != nullptr && bus->isEnabled())
        {
            auto sc = getBusBuffer (buffer, true, 1);
            if (sc.getNumChannels() > 0 && n <= scScratch.getNumSamples())
            {
                scScratch.copyFrom (0, 0, sc, 0, 0, n);
                scScratch.copyFrom (1, 0, sc, sc.getNumChannels() > 1 ? 1 : 0, 0, n);
                scL = scScratch.getReadPointer (0);
                scR = scScratch.getReadPointer (1);
            }
        }
    // mono input feeds both sides
    if (getMainBusNumInputChannels() == 1) buffer.copyFrom (1, 0, buffer, 0, 0, n);
    scActive.store (scL != nullptr);

    double bpm = 120.0, ppq = 0.0;
    bool hostPlaying = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto q = pos->getPpqPosition(); q && pos->getIsPlaying()) { ppq = *q; hostPlaying = true; }
        }
    // stopped transport: one free-running clock shared by every slot keeps them in step
    if (! hostPlaying) ppq = freeRunPpq;
    freeRunPpq = (hostPlaying ? ppq : freeRunPpq) + (double) n * bpm / (60.0 * currentSampleRate);

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    for (int s = 0; s < kSlots; ++s)
    {
        auto* fx = slots[(size_t) s].active.load (std::memory_order_acquire);
        if (fx == nullptr) continue;
        // a rebuild is pending (the knobs already belong to the new effect)
        if (fx->getId() != getSlotEffectId (s)) continue;

        float norm[kMaxParams], depth[kMaxParams], data[efx::kDataSize];
        for (int k = 0; k < kMaxParams; ++k)
        {
            norm[k] = knobRaw[s][k] != nullptr ? knobRaw[s][k]->load() : 0.5f;
            depth[k] = macroDepth[s][k].load (std::memory_order_relaxed);
        }
        for (int i = 0; i < efx::kDataSize; ++i) data[i] = slotData[s][i].load (std::memory_order_relaxed);
        efx::SlotInput in;
        in.norm = norm;
        in.macroDepth = depth;
        in.macro = macroRaw[s] != nullptr ? macroRaw[s]->load() : 0.0f;
        in.mix = mixRaw[s] != nullptr ? mixRaw[s]->load() : 1.0f;
        in.mode = getSlotMode (s);
        in.bypass = bypassParam[s] != nullptr && bypassParam[s]->get();
        in.bpm = bpm;
        in.ppq = ppq;
        in.playing = true;   // ppq is always valid (host or free-running clock)
        in.scL = scL; in.scR = scR;
        in.data = data; in.dataGen = dataGen[s].load (std::memory_order_relaxed);
        fx->process (L, R, n, in);
    }
}

// ------------------------------------------------------------------ state
namespace
{
    juce::String floatsToString (const float* v, int n)
    {
        juce::StringArray a;
        for (int i = 0; i < n; ++i) a.add (juce::String (v[i], 5));
        return a.joinIntoString (" ");
    }
    int stringToFloats (const juce::String& str, float* out, int n)
    {
        juce::StringArray a;
        a.addTokens (str, " ", "");
        const int m = juce::jmin (n, a.size());
        for (int i = 0; i < m; ++i) out[i] = a[i].getFloatValue();
        return m;
    }
}

void EfxGridProcessor::writeExtrasToState()
{
    auto state = apvts.state;
    state.setProperty ("version", kStateVersion, nullptr);
    state.removeChild (state.getChildWithName ("EXTRAS"), nullptr);
    juce::ValueTree extras ("EXTRAS");
    for (int s = 0; s < kSlots; ++s)
    {
        juce::ValueTree slot ("SLOT");
        slot.setProperty ("index", s, nullptr);
        float d[kMaxParams];
        for (int k = 0; k < kMaxParams; ++k) d[k] = macroDepth[s][k].load();
        slot.setProperty ("macroDepth", floatsToString (d, kMaxParams), nullptr);
        if (auto* fx = efx::defForId (getSlotEffectId (s)); fx != nullptr)
        {
            float data[efx::kDataSize];
            getSlotDataAll (s, data);
            slot.setProperty ("data", floatsToString (data, efx::kDataSize), nullptr);
        }
        if (const auto& t = tables[(size_t) s]; t != nullptr)
        {
            // embed the table (up to 64 frames, 16-bit) so the session is self-contained
            const int frames = juce::jmin (64, t->numFrames);
            juce::MemoryBlock mb ((size_t) frames * efx::Wavetable::kFrameLen * 2);
            auto* dst = static_cast<juce::int16*> (mb.getData());
            for (int f = 0; f < frames; ++f)
            {
                const int src = t->numFrames > 64 ? f * (t->numFrames - 1) / 63 : f;
                const float* fr = t->frame (src);
                for (int i = 0; i < efx::Wavetable::kFrameLen; ++i)
                    dst[f * efx::Wavetable::kFrameLen + i] = (juce::int16) juce::jlimit (-32767, 32767, juce::roundToInt (fr[i] * 32767.0f));
            }
            slot.setProperty ("tableName", t->name, nullptr);
            slot.setProperty ("tableFrames", frames, nullptr);
            slot.setProperty ("tableData", mb.toBase64Encoding(), nullptr);
        }
        if (const auto& a = assets[(size_t) s]; a != nullptr)
        {
            // embed the IR (up to 8 s, 16-bit, peak-normalised)
            const int ch = a->buf.getNumChannels();
            const int len = juce::jmin (a->buf.getNumSamples(), (int) (a->sampleRate * 8.0));
            float pk = 1.0e-6f;
            for (int c = 0; c < ch; ++c) pk = juce::jmax (pk, a->buf.getMagnitude (c, 0, len));
            juce::MemoryBlock mb ((size_t) len * (size_t) ch * 2);
            auto* dst = static_cast<juce::int16*> (mb.getData());
            for (int i = 0; i < len; ++i)
                for (int c = 0; c < ch; ++c)
                    dst[i * ch + c] = (juce::int16) juce::jlimit (-32767, 32767, juce::roundToInt (a->buf.getSample (c, i) / pk * 32767.0f));
            slot.setProperty ("irName", a->name, nullptr);
            slot.setProperty ("irRate", a->sampleRate, nullptr);
            slot.setProperty ("irChannels", ch, nullptr);
            slot.setProperty ("irData", mb.toBase64Encoding(), nullptr);
        }
        extras.appendChild (slot, nullptr);
    }
    state.appendChild (extras, nullptr);
}

void EfxGridProcessor::readExtrasFromState()
{
    const auto extras = apvts.state.getChildWithName ("EXTRAS");
    for (int s = 0; s < kSlots; ++s)
    {
        const auto d0 = defaultState (getSlotEffectId (s));
        for (int k = 0; k < kMaxParams; ++k) macroDepth[s][k].store (d0.depth[k]);
        for (int i = 0; i < efx::kDataSize; ++i) slotData[s][i].store (d0.data[i]);
        ++dataGen[s];
        setTable (s, nullptr);
        setAsset (s, nullptr);
    }
    for (const auto& slot : extras)
    {
        const int s = slot.getProperty ("index", -1);
        if (s < 0 || s >= kSlots) continue;
        float d[kMaxParams];
        const int nd = stringToFloats (slot.getProperty ("macroDepth").toString(), d, kMaxParams);
        for (int k = 0; k < nd; ++k) macroDepth[s][k].store (juce::jlimit (-1.0f, 1.0f, d[k]));
        if (slot.hasProperty ("data"))
        {
            float data[efx::kDataSize];
            getSlotDataAll (s, data);
            stringToFloats (slot.getProperty ("data").toString(), data, efx::kDataSize);
            for (int i = 0; i < efx::kDataSize; ++i) slotData[s][i].store (data[i]);
            ++dataGen[s];
        }
        if (slot.hasProperty ("tableData"))
        {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (slot.getProperty ("tableData").toString()))
            {
                const int count = (int) (mb.getSize() / 2);
                std::vector<float> data ((size_t) count);
                const auto* src = static_cast<const juce::int16*> (mb.getData());
                for (int i = 0; i < count; ++i) data[(size_t) i] = (float) src[i] / 32767.0f;
                setTable (s, efx::Wavetable::fromSamples (data.data(), count, efx::Wavetable::kFrameLen, slot.getProperty ("tableName").toString()));
            }
        }
        if (slot.hasProperty ("irData"))
        {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (slot.getProperty ("irData").toString()))
            {
                const int ch = juce::jlimit (1, 2, (int) slot.getProperty ("irChannels", 1));
                const int len = (int) (mb.getSize() / 2) / ch;
                auto a = std::make_shared<efx::AudioAsset>();
                a->buf.setSize (ch, juce::jmax (1, len));
                const auto* src = static_cast<const juce::int16*> (mb.getData());
                for (int i = 0; i < len; ++i)
                    for (int c = 0; c < ch; ++c) a->buf.setSample (c, i, (float) src[i * ch + c] / 32767.0f);
                a->sampleRate = (double) slot.getProperty ("irRate", 48000.0);
                a->name = slot.getProperty ("irName").toString();
                setAsset (s, a);
            }
        }
    }
}

// Sessions saved with the v2 control layout: carry each control over by name,
// through its physical value (so -18 dB stays -18 dB even if the range moved),
// choices by label; anything without a match takes the new default.
void EfxGridProcessor::migrateV2Slot (int slot, const float* oldNorm, int oldMode, const float* oldDepth)
{
    const int fx = getSlotEffectId (slot);
    if (fx <= 0) return;
    auto st = defaultState (fx);
    st.mix = mixRaw[slot] != nullptr ? mixRaw[slot]->load() : st.mix;
    st.macro = macroRaw[slot] != nullptr ? macroRaw[slot]->load() : 0.0f;
    st.bypass = bypassParam[slot] != nullptr && bypassParam[slot]->get();
    st.table = tables[(size_t) slot];
    if (fx <= efx::kNumLegacyV2)
    {
        const auto& od = efx::kLegacyV2[fx - 1];
        const auto& nd = *efx::defForId (fx);
        for (int k = 0; k < nd.numParams; ++k)
            for (int j = 0; j < od.numParams; ++j)
            {
                if (! juce::String (od.p[j].name).equalsIgnoreCase (nd.p[k].name)) continue;
                const auto& os = od.p[j];
                const auto& ns = nd.p[k];
                if (os.isChoice() && ns.isChoice())
                {
                    const int oi = (int) efx::unmapParam (os, oldNorm[j]);
                    for (int c = 0; c < ns.numChoices; ++c)
                        if (oi >= 0 && oi < os.numChoices && juce::String (ns.choices[c]) == os.choices[oi]) st.p[k] = efx::mapParam (ns, (float) c);
                }
                else if (! os.isChoice() && ! ns.isChoice())
                    st.p[k] = efx::mapParam (ns, efx::unmapParam (os, oldNorm[j]));
                if (oldDepth != nullptr) st.depth[k] = oldDepth[j];
                break;
            }
        if (od.modes != nullptr && oldMode >= 0 && oldMode < od.numModes)
            for (int m = 0; m < nd.numModes; ++m)
                if (juce::String (nd.modes[m]) == od.modes[oldMode]) st.mode = m;
    }
    setSlotState (slot, st);
}

void EfxGridProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = presetXml())
        copyXmlToBinary (*xml, destData);
}

std::unique_ptr<juce::XmlElement> EfxGridProcessor::presetXml()
{
    writeExtrasToState();
    apvts.state.setProperty ("presetName", presetName, nullptr);
    return apvts.copyState().createXml();
}

void EfxGridProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! applyStateXml (*xml)) return;
    clearUndo();   // a loaded session starts a fresh history
}

bool EfxGridProcessor::loadPresetXml (const juce::XmlElement& xml)
{
    if (! xml.hasTagName (apvts.state.getType())) return false;
    UndoBatch batch (*this);
    return applyStateXml (xml);
}

bool EfxGridProcessor::applyStateXml (const juce::XmlElement& xmlIn)
{
    const auto* xml = &xmlIn;
    if (! xml->hasTagName (apvts.state.getType())) return false;
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    const int version = (int) apvts.state.getProperty ("version", 1);
    if (version < 3)
    {
        // read what v2 stored (knob values by index, macro routing) before rewriting the slots
        float oldNorm[kSlots][kMaxParams], oldDepth[kSlots][kMaxParams];
        int oldMode[kSlots];
        bool haveDepth[kSlots] {};
        for (int s = 0; s < kSlots; ++s)
        {
            for (int k = 0; k < kMaxParams; ++k) { oldNorm[s][k] = getNorm (s, k); oldDepth[s][k] = 0.0f; }
            oldMode[s] = modeChoice[s] != nullptr ? modeChoice[s]->getIndex() : 0;
        }
        for (const auto& slot : apvts.state.getChildWithName ("EXTRAS"))
        {
            const int s = slot.getProperty ("index", -1);
            if (s < 0 || s >= kSlots) continue;
            haveDepth[s] = stringToFloats (slot.getProperty ("macroDepth").toString(), oldDepth[s], kMaxParams) > 0;
        }
        readExtrasFromState();   // wavetables
        for (int s = 0; s < kSlots; ++s)
        {
            if (version < 2) { if (getSlotEffectId (s) > 0) { auto st = defaultState (getSlotEffectId (s)); st.mix = mixRaw[s]->load(); setSlotState (s, st); } }
            else migrateV2Slot (s, oldNorm[s], oldMode[s], haveDepth[s] ? oldDepth[s] : nullptr);
        }
        apvts.state.setProperty ("version", kStateVersion, nullptr);
    }
    else readExtrasFromState();
    if (version < 4) migrateV3Slots();
    apvts.state.setProperty ("version", kStateVersion, nullptr);
    for (int s = 0; s < kSlots; ++s) pending[s].store (true);
    anyPending.store (true);
    triggerAsyncUpdate();
    ++stateGen;
    presetName = apvts.state.getProperty ("presetName", "Init").toString();
    if (onSlotsChanged) onSlotsChanged();
    return true;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EfxGridProcessor();
}

// ------------------------------------------------------------------ undo / redo
EfxGridProcessor::Snapshot EfxGridProcessor::takeSnapshot() const
{
    Snapshot sn;
    for (int s = 0; s < kSlots; ++s) sn[(size_t) s] = getSlotState (s);
    return sn;
}

bool EfxGridProcessor::sameSlot (const SlotState& a, const SlotState& b)
{
    if (a.fx != b.fx || a.mode != b.mode || a.bypass != b.bypass || a.table != b.table || a.asset != b.asset) return false;
    if (std::abs (a.macro - b.macro) > 1.0e-6f || std::abs (a.mix - b.mix) > 1.0e-6f) return false;
    for (int k = 0; k < kMaxParams; ++k)
        if (std::abs (a.p[k] - b.p[k]) > 1.0e-6f || std::abs (a.depth[k] - b.depth[k]) > 1.0e-6f) return false;
    for (int i = 0; i < efx::kDataSize; ++i)
        if (std::abs (a.data[i] - b.data[i]) > 0.0f) return false;
    return true;
}

void EfxGridProcessor::applySnapshot (const Snapshot& sn)
{
    for (int s = 0; s < kSlots; ++s)
        if (! sameSlot (getSlotState (s), sn[(size_t) s]))
            setSlotState (s, sn[(size_t) s]);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    if (onSlotsChanged) onSlotsChanged();
}

void EfxGridProcessor::undoBaseline()
{
    undoBase = std::make_unique<Snapshot> (takeSnapshot());
}

void EfxGridProcessor::undoCheckpoint (bool mergeRecent)
{
    if (undoHold > 0) return;   // inside a batch: one step when it ends
    auto now = std::make_unique<Snapshot> (takeSnapshot());
    if (undoBase == nullptr) { undoBase = std::move (now); return; }
    bool changed = false;
    for (int s = 0; s < kSlots && ! changed; ++s) changed = ! sameSlot ((*undoBase)[(size_t) s], (*now)[(size_t) s]);
    if (! changed) return;
    const auto t = juce::Time::getMillisecondCounter();
    // a quick run of small edits (scroll wheel) folds into the last step
    if (! (mergeRecent && ! undoStack.empty() && redoStack.empty() && t - lastPushMs < 800))
    {
        undoStack.push_back (*undoBase);
        if (undoStack.size() > kUndoDepth) undoStack.erase (undoStack.begin());
    }
    lastPushMs = t;
    redoStack.clear();
    undoBase = std::move (now);
}

bool EfxGridProcessor::undoStep()
{
    undoCheckpoint();   // an edit in progress becomes its own step first
    if (undoStack.empty()) return false;
    redoStack.push_back (undoBase != nullptr ? *undoBase : takeSnapshot());
    const auto target = undoStack.back();
    undoStack.pop_back();
    applySnapshot (target);
    undoBase = std::make_unique<Snapshot> (takeSnapshot());
    return true;
}

bool EfxGridProcessor::redoStep()
{
    undoCheckpoint();
    if (redoStack.empty()) return false;
    undoStack.push_back (undoBase != nullptr ? *undoBase : takeSnapshot());
    const auto target = redoStack.back();
    redoStack.pop_back();
    applySnapshot (target);
    undoBase = std::make_unique<Snapshot> (takeSnapshot());
    return true;
}

void EfxGridProcessor::clearUndo()
{
    undoStack.clear();
    redoStack.clear();
    undoBase = std::make_unique<Snapshot> (takeSnapshot());
}

// v3 -> v4: effect #19 was the ADSR and is now the Harmonic Resonator. Old
// ADSR slots become Transient Shapers (keeping decay / release, mix, bypass);
// Transient Shapers gain a Decay control in a slot that used to be unused.
void EfxGridProcessor::migrateV3Slots()
{
    const int resId = efx::idForName ("Harmonic Resonator"), tsId = efx::idForName ("Transient Shaper");
    const auto* ts = efx::defForId (tsId);
    if (ts == nullptr) return;
    auto logMs = [] (float x01, float lo, float hi) { return lo * std::pow (hi / lo, juce::jlimit (0.0f, 1.0f, x01)); };
    for (int s = 0; s < kSlots; ++s)
    {
        const auto old = getSlotState (s);
        if (old.fx == resId)
        {
            auto st = defaultState (tsId);
            st.p[6] = efx::mapParam (ts->p[6], logMs (old.p[1], 1.0f, 2000.0f));   // ADSR Decay 1..2000 ms
            st.p[5] = efx::mapParam (ts->p[5], logMs (old.p[3], 1.0f, 4000.0f));   // ADSR Release 1..4000 ms
            st.mix = old.mix; st.bypass = old.bypass;
            setSlotState (s, st);
        }
        else if (old.fx == tsId)
            setParamNorm (paramId (s, "p6"), ts->p[6].def);
    }
}

// ------------------------------------------------------------------ presets
void EfxGridProcessor::applyChain (const std::vector<std::pair<int, std::vector<std::pair<juce::String, float>>>>& chain,
                                   const std::vector<int>& modes, const std::vector<float>& mixes)
{
    UndoBatch batch (*this);
    for (int s = 0; s < kSlots; ++s) setSlotState (s, defaultState (0));
    for (size_t i = 0; i < chain.size() && (int) i < kSlots; ++i)
    {
        const int slot = (int) i, id = chain[i].first;
        const auto* d = efx::defForId (id);
        if (d == nullptr) continue;
        auto st = defaultState (id);
        for (const auto& [name, phys] : chain[i].second)
            for (int k = 0; k < d->numParams; ++k)
                if (juce::String (d->p[k].name) == name) st.p[k] = efx::mapParam (d->p[k], phys);
        if (i < modes.size()) st.mode = juce::jlimit (0, juce::jmax (0, d->numModes - 1), modes[i]);
        if (i < mixes.size() && mixes[i] >= 0.0f) st.mix = mixes[i];
        setSlotState (slot, st);
    }
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    if (onSlotsChanged) onSlotsChanged();
}

// ------------------------------------------------------------------ randomize / mutate
// Knobs move around their defaults (randomize) or around where they are
// (mutate). Meters are left alone, and so are output levels (no surprise
// jumps), file-backed choices and the slot's drawn shapes.
static bool keepOnRandomize (const efx::EffectDef& d, int k)
{
    const juce::String n (d.p[k].name);
    if (n == "Out" || n == "Gain" || n == "Makeup" || n == "Ceiling" || n == "Input" || n == "Listen" || n == "Learn" || n == "Reset")
        return true;
    return false;
}

static int randomChoice (const efx::ParamSpec& sp, juce::Random& rng)
{
    int n = sp.numChoices;
    // never land on an entry that needs a file the user hasn't loaded
    if (n > 1 && sp.choices != nullptr && juce::String (sp.choices[n - 1]) == "User File") --n;
    return rng.nextInt (juce::jmax (1, n));
}

void EfxGridProcessor::randomizeSlotNoUndo (int slot, float intensity)
{
    const auto* d = efx::defForId (getSlotEffectId (slot));
    if (d == nullptr || d->cat == efx::CatAnalysis) return;
    auto st = getSlotState (slot);
    for (int k = 0; k < d->numParams; ++k)
    {
        if (keepOnRandomize (*d, k)) continue;
        const auto& sp = d->p[k];
        if (sp.isChoice())
        {
            if (rng.nextFloat() < 0.5f * intensity) st.p[k] = sp.numChoices > 1 ? (float) randomChoice (sp, rng) / (float) (sp.numChoices - 1) : 0.0f;
            continue;
        }
        const float wild = sp.def + (rng.nextFloat() * 2.0f - 1.0f) * 0.5f * intensity;
        st.p[k] = juce::jlimit (0.0f, 1.0f, st.p[k] * (1.0f - intensity) + wild * intensity);
    }
    if (d->numModes > 1 && rng.nextFloat() < 0.3f * intensity)
    {
        const int m = rng.nextInt (d->numModes);
        if (juce::String (d->modes[m]) != "Ext") st.mode = m;   // a sidechain mode needs a key input
    }
    setSlotState (slot, st);
}

void EfxGridProcessor::mutateSlotNoUndo (int slot, float amount)
{
    const auto* d = efx::defForId (getSlotEffectId (slot));
    if (d == nullptr || d->cat == efx::CatAnalysis) return;
    auto st = getSlotState (slot);
    for (int k = 0; k < d->numParams; ++k)
    {
        if (keepOnRandomize (*d, k)) continue;
        const auto& sp = d->p[k];
        if (sp.isChoice())
        {
            if (rng.nextFloat() < 0.08f * amount * 4.0f) st.p[k] = sp.numChoices > 1 ? (float) randomChoice (sp, rng) / (float) (sp.numChoices - 1) : 0.0f;
            continue;
        }
        st.p[k] = juce::jlimit (0.0f, 1.0f, st.p[k] + (rng.nextFloat() * 2.0f - 1.0f) * 0.25f * amount);
    }
    setSlotState (slot, st);
}

void EfxGridProcessor::randomizeSlot (int slot, float intensity)
{
    UndoBatch batch (*this);
    randomizeSlotNoUndo (slot, intensity);
    presetName = "Randomized"; presetIndex = -1;
}

void EfxGridProcessor::mutateSlot (int slot, float amount)
{
    UndoBatch batch (*this);
    mutateSlotNoUndo (slot, amount);
    presetName = "Mutated"; presetIndex = -1;
}

void EfxGridProcessor::randomizeChain (float intensity)
{
    UndoBatch batch (*this);
    for (int s = 0; s < chainLength(); ++s) randomizeSlotNoUndo (s, intensity);
    presetName = "Randomized"; presetIndex = -1;
}

void EfxGridProcessor::mutateChain (float amount)
{
    UndoBatch batch (*this);
    for (int s = 0; s < chainLength(); ++s) mutateSlotNoUndo (s, amount);
    presetName = "Mutated"; presetIndex = -1;
}

void EfxGridProcessor::randomNewChain()
{
    UndoBatch batch (*this);
    std::vector<int> pool;
    for (int id = 1; id <= efx::kNumEffects; ++id)
        if (const auto* d = efx::defForId (id); d != nullptr && d->cat != efx::CatAnalysis) pool.push_back (id);
    const int n = 3 + rng.nextInt (3);
    for (int s = 0; s < kSlots; ++s) setSlotState (s, defaultState (0));
    for (int s = 0; s < n; ++s)
    {
        const int id = pool[(size_t) rng.nextInt ((int) pool.size())];
        auto st = defaultState (id);
        // time / space effects in a random chain: keep them in the background
        if (const auto* d = efx::defForId (id); d->cat == efx::CatSpace || d->cat == efx::CatTime) st.mix = juce::jmin (st.mix, 0.35f);
        setSlotState (s, st);
        randomizeSlotNoUndo (s, 0.5f);
    }
    presetName = "Random Chain"; presetIndex = -1;
    if (onSlotsChanged) onSlotsChanged();
}
