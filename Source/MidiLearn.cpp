#include "MidiLearn.h"

namespace pastel
{
static const char* encName(MidiLearn::Enc e)
{
    return e == MidiLearn::Enc::offset64 ? "offset64" : e == MidiLearn::Enc::twos ? "twos" : "absolute";
}

static MidiLearn::Enc encFromName(const juce::String& s)
{
    return s == "offset64" ? MidiLearn::Enc::offset64 : s == "twos" ? MidiLearn::Enc::twos : MidiLearn::Enc::absolute;
}

MidiLearn::MidiLearn(juce::AudioProcessor& processor, const juce::String& pluginName)
    : proc(processor), file(fileFor(pluginName))
{
    for (auto& a : ccIndex) a.store(-1);
    for (auto& a : ccEnc) a.store((int)Enc::absolute);
    for (auto& a : heardVals) a.store(0);
    params = proc.getParameters();
    for (auto* p : params)
        p->addListener(this);
    load();
    startTimerHz(20);
}

MidiLearn::~MidiLearn()
{
    stopTimer();
    for (auto* p : params)
        p->removeListener(this);
}

static juce::File& testDir()
{
    static juce::File dir;
    return dir;
}

void MidiLearn::setDirectoryForTest(const juce::File& dir) { testDir() = dir; }

juce::File MidiLearn::fileFor(const juce::String& pluginName)
{
    auto dir = testDir() != juce::File() ? testDir()
             : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("PastelMidiLearn");
    return dir.getChildFile(juce::File::createLegalFileName(pluginName) + ".xml");
}

MidiLearn::Enc MidiLearn::detectEncoding(const std::vector<int>& v)
{
    if (v.empty())
        return Enc::absolute;
    auto twosLike = [](int x) { return (x >= 1 && x <= 15) || x >= 113; };
    auto offLike = [](int x) { return x >= 49 && x <= 79 && x != 64; };
    bool allTwos = true, allOff = true, repeats = false, monotonic = true;
    for (size_t i = 0; i < v.size(); ++i)
    {
        allTwos = allTwos && twosLike(v[i]);
        allOff = allOff && offLike(v[i]);
        if (i > 0)
        {
            repeats = repeats || v[i] == v[i - 1];
            if (i > 1 && ((v[i] - v[i - 1]) > 0) != ((v[i - 1] - v[i - 2]) > 0))
                monotonic = false;
        }
    }
    // a turned absolute knob walks through distinct values in one direction;
    // a relative encoder repeats the same "step" value around its centre
    if (v.size() == 1 || repeats || !monotonic)
    {
        if (allTwos) return Enc::twos;
        if (allOff) return Enc::offset64;
    }
    return Enc::absolute;
}

int MidiLearn::relativeDelta(Enc enc, int value)
{
    if (enc == Enc::offset64) return value - 64;
    if (enc == Enc::twos) return value < 64 ? value : value - 128;
    return 0;
}

void MidiLearn::process(const juce::MidiBuffer& midi)
{
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (!msg.isController())
            continue;
        const int cc = msg.getControllerNumber(), v = msg.getControllerValue();

        if (learning.load() && armed.load() != -1)
        {
            int none = -1;
            heardCC.compare_exchange_strong(none, cc);
            if (heardCC.load() == cc)
            {
                int n = heardCount.load();
                if (n < kLearnSamples)
                {
                    heardVals[(size_t)n].store(v);
                    heardCount.store(n + 1);
                }
            }
            continue; // nothing moves while a control is waiting for its knob
        }

        if (cc == presetCC.load())
        {
            auto enc = (Enc)presetEnc.load();
            int d = 0;
            if (enc == Enc::absolute)
            {
                if (lastPresetAbs >= 0) d = v - lastPresetAbs;
                lastPresetAbs = v;
            }
            else
                d = relativeDelta(enc, v);
            if (d != 0)
                pendingSteps += d > 0 ? 1 : -1; // one preset per detent, whatever the acceleration
            continue;
        }

        const int idx = ccIndex[(size_t)cc].load();
        if (idx < 0 || idx >= params.size())
            continue;
        auto* p = params.getUnchecked(idx);
        auto enc = (Enc)ccEnc[(size_t)cc].load();
        if (enc == Enc::absolute)
        {
            p->setValueNotifyingHost((float)v / 127.0f);
        }
        else
        {
            const int steps = p->getNumSteps();
            const float unit = (steps > 1 && steps < 128) ? 1.0f / (float)(steps - 1) : 0.01f;
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, p->getValue() + (float)relativeDelta(enc, v) * unit));
        }
    }
}

void MidiLearn::parameterGestureChanged(int index, bool starting)
{
    if (!learning.load() || !starting || armed.load() == index)
        return;
    armed.store(index);
    heardCount.store(0);
    heardCC.store(-1);
    heardTicks = 0;
    changed();
}

void MidiLearn::setLearning(bool on)
{
    learning.store(on);
    armed.store(-1);
    heardCount.store(0);
    heardCC.store(-1);
    heardTicks = 0;
    changed();
}

void MidiLearn::armPresetKnob()
{
    learning.store(true);
    armed.store(kPresetArmed);
    heardCount.store(0);
    heardCC.store(-1);
    heardTicks = 0;
    changed();
}

juce::String MidiLearn::getArmedName() const
{
    const int a = armed.load();
    if (a == kPresetArmed) return "Preset knob";
    if (a >= 0 && a < params.size()) return params.getUnchecked(a)->getName(40);
    return {};
}

std::vector<MidiLearn::Mapping> MidiLearn::getMappings() const
{
    std::vector<Mapping> out;
    for (auto& [cc, e] : ccMap)
    {
        const int idx = indexOf(e.paramId);
        out.push_back({ cc, e.paramId, idx >= 0 ? params.getUnchecked(idx)->getName(40) : e.paramId, e.enc });
    }
    return out;
}

void MidiLearn::clearCC(int cc)
{
    ccMap.erase(cc);
    rebuildIndex();
    save();
    changed();
}

void MidiLearn::clearPresetKnob()
{
    presetCC.store(-1);
    save();
    changed();
}

void MidiLearn::clearAll()
{
    ccMap.clear();
    presetCC.store(-1);
    rebuildIndex();
    save();
    changed();
}

void MidiLearn::tick()
{
    if (heardCC.load() >= 0)
    {
        // bind once a few values are in, or shortly after a single nudge
        if (heardCount.load() >= kLearnSamples || ++heardTicks >= 8)
            finishLearn();
    }

    if (int steps = pendingSteps.exchange(0); steps != 0 && onPresetStep)
        onPresetStep(steps);

    // another instance (or process) saved new mappings: pick them up
    if (++fileTicks >= 20)
    {
        fileTicks = 0;
        if (file.existsAsFile() && file.getLastModificationTime() != fileTime)
        {
            load();
            changed();
        }
    }
}

void MidiLearn::finishLearn()
{
    const int target = armed.load();
    const int cc = heardCC.load();
    std::vector<int> vals;
    for (int i = 0; i < juce::jmin(heardCount.load(), kLearnSamples); ++i)
        vals.push_back(heardVals[(size_t)i].load());
    armed.store(-1);
    heardCount.store(0);
    heardCC.store(-1);
    heardTicks = 0;
    if (cc < 0 || target == -1)
        return;

    const auto enc = detectEncoding(vals);
    if (target == kPresetArmed)
    {
        ccMap.erase(cc);
        presetEnc.store((int)enc);
        presetCC.store(cc);
        lastLearned = "CC " + juce::String(cc) + " now browses presets";
    }
    else if (target >= 0 && target < params.size())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(params.getUnchecked(target));
        if (withId == nullptr)
            return;
        // one knob per control: drop the control's old CC
        for (auto it = ccMap.begin(); it != ccMap.end();)
            it = it->second.paramId == withId->paramID ? ccMap.erase(it) : std::next(it);
        ccMap[cc] = { withId->paramID, enc };
        if (presetCC.load() == cc)
            presetCC.store(-1);
        lastLearned = "CC " + juce::String(cc) + " now controls " + withId->getName(40);
    }
    rebuildIndex();
    save();
    changed();
}

int MidiLearn::indexOf(const juce::String& paramId) const
{
    for (int i = 0; i < params.size(); ++i)
        if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*>(params.getUnchecked(i)))
            if (p->paramID == paramId)
                return i;
    return -1;
}

void MidiLearn::rebuildIndex()
{
    for (int cc = 0; cc < 128; ++cc)
    {
        auto it = ccMap.find(cc);
        const int idx = it != ccMap.end() ? indexOf(it->second.paramId) : -1;
        ccEnc[(size_t)cc].store((int)(it != ccMap.end() ? it->second.enc : Enc::absolute));
        ccIndex[(size_t)cc].store(idx);
    }
}

void MidiLearn::load()
{
    ccMap.clear();
    presetCC.store(-1);
    if (auto xml = juce::XmlDocument::parse(file))
    {
        if (xml->hasTagName("MidiLearn"))
        {
            const int pk = xml->getIntAttribute("presetKnob", -1);
            presetEnc.store((int)encFromName(xml->getStringAttribute("presetEncoder")));
            presetCC.store(pk >= 0 && pk < 128 ? pk : -1);
            for (auto* m : xml->getChildWithTagNameIterator("Map"))
            {
                const int cc = m->getIntAttribute("cc", -1);
                const auto id = m->getStringAttribute("param");
                if (cc >= 0 && cc < 128 && id.isNotEmpty())
                    ccMap[cc] = { id, encFromName(m->getStringAttribute("enc")) };
            }
        }
    }
    fileTime = file.getLastModificationTime();
    rebuildIndex();
}

void MidiLearn::save()
{
    juce::XmlElement xml("MidiLearn");
    if (presetCC.load() >= 0)
    {
        xml.setAttribute("presetKnob", presetCC.load());
        xml.setAttribute("presetEncoder", encName((Enc)presetEnc.load()));
    }
    for (auto& [cc, e] : ccMap)
    {
        auto* m = xml.createNewChildElement("Map");
        m->setAttribute("cc", cc);
        m->setAttribute("param", e.paramId);
        m->setAttribute("enc", encName(e.enc));
    }
    file.getParentDirectory().createDirectory();
    xml.writeTo(file);
    fileTime = file.getLastModificationTime();
}

//==============================================================================
MidiLearnButton::MidiLearnButton(MidiLearn& ml)
    : juce::TextButton("MIDI"), learn(ml)
{
    setTooltip("MIDI learn: map your keyboard's knobs to any control, for every project");
    startTimerHz(10);
}

MidiLearnButton::~MidiLearnButton()
{
    learn.setLearning(false); // no editor, nothing to touch: don't leave a control waiting
}

void MidiLearnButton::clicked()
{
    enum { idLearn = 1, idPreset, idPresetClear, idClearAll, idShowFile, idMapBase = 100 };
    juce::PopupMenu m;
    m.addItem(idLearn, learn.isLearning() ? "Stop MIDI learn" : "MIDI learn (touch a control, then move a knob)",
              true, learn.isLearning());
    m.addItem(idPreset, "Learn preset knob (turn it to browse presets)");

    auto maps = learn.getMappings();
    const int pk = learn.getPresetKnobCC();
    if (!maps.empty() || pk >= 0)
    {
        m.addSeparator();
        m.addSectionHeader("Click a mapping to remove it");
        if (pk >= 0)
            m.addItem(idPresetClear, "CC " + juce::String(pk) + "  ->  Preset knob");
        for (auto& mp : maps)
            m.addItem(idMapBase + mp.cc, "CC " + juce::String(mp.cc) + "  ->  " + mp.name
                      + (mp.enc != MidiLearn::Enc::absolute ? "  (endless)" : ""));
        m.addItem(idClearAll, "Clear all mappings");
    }
    m.addSeparator();
    m.addItem(idShowFile, "Show mappings file");

    juce::Component::SafePointer<MidiLearnButton> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [safe](int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& l = safe->learn;
        if (r == idLearn) l.setLearning(!l.isLearning());
        else if (r == idPreset) l.armPresetKnob();
        else if (r == idPresetClear) l.clearPresetKnob();
        else if (r == idClearAll) l.clearAll();
        else if (r == idShowFile) { if (l.getFile().existsAsFile()) l.getFile().revealToUser(); else l.getFile().getParentDirectory().revealToUser(); }
        else if (r >= idMapBase) l.clearCC(r - idMapBase);
    });
}

void MidiLearnButton::timerCallback()
{
    const bool on = learn.isLearning();
    if (getToggleState() != on)
    {
        setToggleState(on, juce::dontSendNotification);
        setButtonText(on ? "LEARN" : "MIDI");
        if (on)
            hint("MIDI learn: touch a control on screen", 6000);
        else
            bubble.setVisible(false);
    }
    if (learn.getRevision() == seenRevision)
        return;
    seenRevision = learn.getRevision();

    const auto armedName = learn.getArmedName();
    if (armedName != seenArmed)
    {
        seenArmed = armedName;
        if (armedName == "Preset knob")
            hint("Turn the knob you want for browsing presets", 10000);
        else if (armedName.isNotEmpty())
            hint(armedName + ": now move a knob or fader on your keyboard", 10000);
        else if (on && learn.getLastLearned().isNotEmpty())
            hint(learn.getLastLearned() + ". Touch another control to map more.", 5000);
    }
}

void MidiLearnButton::hint(const juce::String& message, int ms)
{
    auto* top = getTopLevelComponent();
    if (top == nullptr || !isShowing())
        return;
    if (bubble.getParentComponent() != top)
        top->addChildComponent(bubble);
    bubble.setColour(juce::BubbleComponent::backgroundColourId, findColour(juce::TextButton::buttonColourId).brighter(0.15f));
    bubble.setColour(juce::BubbleComponent::outlineColourId, findColour(juce::TextButton::textColourOffId).withAlpha(0.6f));
    juce::AttributedString s;
    s.append(message, juce::FontOptions(14.0f), findColour(juce::TextButton::textColourOffId));
    s.setJustification(juce::Justification::centred);
    bubble.showAt(this, s, ms, true, false);
    bubble.toFront(false);
}
} // namespace pastel
