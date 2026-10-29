#include "PluginEditor.h"
#include "Theme.h"
#include "Visuals.h"

// ============================================================
// helpers
// ============================================================
namespace
{
    juce::Font fnt (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    juce::String slotLabel (int slot) { return juce::String (slot + 1).paddedLeft ('0', 2); }

    juce::Colour accentForSlot (const EfxGridProcessor& proc, int slot)
    {
        const auto* d = efx::defForId (proc.getSlotEffectId (slot));
        return d != nullptr ? efx::catAccent ((int) d->cat) : juce::Colour (0xFF3A3D43);
    }

    bool slotBypassed (EfxGridProcessor& proc, int slot)
    {
        if (auto* p = proc.apvts.getRawParameterValue (EfxGridProcessor::paramId (slot, "bypass"))) return p->load() > 0.5f;
        return false;
    }

    void setParam (EfxGridProcessor& proc, int slot, const char* suffix, float norm)
    {
        if (auto* p = proc.apvts.getParameter (EfxGridProcessor::paramId (slot, suffix)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, norm));
            p->endChangeGesture();
        }
    }

    void styleSmallButton (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::textColourOffId, PastelColors::text);
        b.setColour (juce::TextButton::textColourOnId, PastelColors::barText);
    }

    struct TileLayout { juce::Rectangle<float> head, viz, macro, expand, power; };

    // head row, then the body split in half: visual | macro knob side by side
    // on wide tiles, stacked on tall ones
    TileLayout tileLayout (juce::Rectangle<float> r)
    {
        TileLayout L;
        auto rr = r.reduced (10.0f, 8.0f);
        L.head = rr.removeFromTop (20.0f);
        L.power = { L.head.getRight() - 15.0f, L.head.getCentreY() - 7.5f, 15.0f, 15.0f };
        L.expand = L.power.translated (-21.0f, 0.0f);
        rr.removeFromTop (6.0f);
        if (rr.getWidth() >= rr.getHeight() * 1.3f)
        {
            L.macro = rr.removeFromRight (rr.getWidth() * 0.5f);
            L.macro.removeFromLeft (8.0f);
        }
        else
        {
            L.macro = rr.removeFromBottom (rr.getHeight() * 0.5f);
            L.macro.removeFromTop (6.0f);
        }
        L.viz = rr;
        return L;
    }

    // the macro knob inside the tile's macro half (leaves room for the caption)
    juce::Rectangle<float> tileKnob (juce::Rectangle<float> m)
    {
        const float d = juce::jlimit (16.0f, 120.0f, juce::jmin (m.getWidth(), m.getHeight() - 18.0f) * 0.86f);
        return juce::Rectangle<float> (d, d).withCentre ({ m.getCentreX(), m.getY() + (m.getHeight() - 18.0f) * 0.5f });
    }
}

// ============================================================
// effect menu
// ============================================================
void showEffectMenu (juce::Component& target, int currentId, bool allowClear, std::function<void (int)> onPick)
{
    juce::PopupMenu menu;
    for (int c = 0; c < efx::NumCats; ++c)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < efx::kNumEffects; ++i)
        {
            const auto& d = efx::effectDef (i);
            if ((int) d.cat != c) continue;
            juce::PopupMenu::Item it (d.name);
            it.itemID = i + 1;
            it.isTicked = (i + 1) == currentId;
            it.colour = efx::catAccent (c);
            sub.addItem (it);
        }
        juce::PopupMenu::Item head (efx::catName (c));
        head.subMenu = std::make_unique<juce::PopupMenu> (sub);
        head.colour = efx::catAccent (c);
        menu.addItem (std::move (head));
    }
    if (allowClear) { menu.addSeparator(); menu.addItem (-1, "Remove effect"); }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                        [cb = std::move (onPick)] (int r) { if (r != 0 && cb) cb (r < 0 ? 0 : r); });
}

// ============================================================
// ParamControl
// ============================================================
class ParamControl::Knob : public juce::Slider
{
public:
    Knob (EfxGridProcessor& p, int s, int k, const efx::ParamSpec& sp) : proc (p), slot (s), idx (k), spec (sp)
    {
        setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 18);
        setDoubleClickReturnValue (true, spec.def);
        setTooltip ("Right-click: macro routing. Double-click: default.");
    }
    bool assign = false;
    std::function<void()> onRouting;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showMenu(); return; }
        if (assign) { startDepth = proc.getMacroDepth (slot, idx); return; }
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (assign)
        {
            const float d = startDepth - (float) e.getDistanceFromDragStartY() / 110.0f + (float) e.getDistanceFromDragStartX() / 220.0f;
            proc.setMacroDepth (slot, idx, d);
            if (onRouting) onRouting();
            return;
        }
        juce::Slider::mouseDrag (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (assign || e.mods.isPopupMenu()) return;
        juce::Slider::mouseUp (e);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (assign) { proc.setMacroDepth (slot, idx, 0.0f); if (onRouting) onRouting(); return; }
        juce::Slider::mouseDoubleClick (e);
    }
    void showMenu()
    {
        const float d = proc.getMacroDepth (slot, idx);
        juce::PopupMenu m;
        m.addSectionHeader (juce::String (spec.name) + "  -  macro " + (std::abs (d) > 0.005f ? juce::String (juce::roundToInt (d * 100.0f)) + "%" : juce::String ("off")));
        m.addItem (1, "Macro pushes this up (+100%)");
        m.addItem (2, "Macro pushes this down (-100%)");
        m.addItem (3, "Macro +50%");
        m.addItem (4, "Remove from macro", std::abs (d) > 0.005f);
        m.addSeparator();
        m.addItem (5, "Reset value to default");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r)
        {
            switch (r)
            {
                case 1: proc.setMacroDepth (slot, idx, 1.0f); break;
                case 2: proc.setMacroDepth (slot, idx, -1.0f); break;
                case 3: proc.setMacroDepth (slot, idx, 0.5f); break;
                case 4: proc.setMacroDepth (slot, idx, 0.0f); break;
                case 5: setValue (spec.def, juce::sendNotificationSync); break;
                default: return;
            }
            if (onRouting) onRouting();
        });
    }
private:
    EfxGridProcessor& proc;
    int slot, idx;
    const efx::ParamSpec& spec;
    float startDepth = 0.0f;
};

ParamControl::ParamControl (EfxGridProcessor& p, int s, int index)
    : proc (p), slot (s), k (index), spec (efx::defForId (p.getSlotEffectId (s))->p[index])
{
    caption.setText (juce::String (spec.name).toUpperCase(), juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (fnt (10.5f, true));
    caption.setColour (juce::Label::textColourId, PastelColors::textDim);
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);

    const auto pid = EfxGridProcessor::paramId (slot, "p" + juce::String (k));
    if (! spec.isChoice())
    {
        knob = std::make_unique<Knob> (proc, slot, k, spec);
        knob->onRouting = [this] { if (onRoutingChanged) onRoutingChanged(); };
        addAndMakeVisible (*knob);
        knobAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, pid, *knob);
        // after the attachment, which installs the host-facing "Name value" strings
        const auto sp = spec;
        knob->textFromValueFunction = [sp] (double v) { return efx::formatParam (sp, (float) v); };
        knob->valueFromTextFunction = [sp, this] (const juce::String& t)
        {
            const float v = efx::parseParam (sp, t);
            return v < 0.0f ? knob->getValue() : (double) v;
        };
        knob->updateText();
        return;
    }

    auto* param = proc.apvts.getParameter (pid);
    const int n = juce::jmax (1, spec.numChoices);
    auto toNorm = [n] (int i) { return n > 1 ? (float) i / (float) (n - 1) : 0.0f; };
    if (juce::String (spec.name) == "Reset")
    {
        // a trigger: every press flips the value, which the effect treats as "reset now"
        auto* b = segs.add (new juce::TextButton ("RESET"));
        styleSmallButton (*b);
        b->onClick = [this] { if (choiceAtt) choiceAtt->setValueAsCompleteGesture (proc.getNorm (slot, k) > 0.5f ? 0.0f : 1.0f); };
        addAndMakeVisible (b);
        choiceAtt = std::make_unique<juce::ParameterAttachment> (*param, [] (float) {});
    }
    else if (n <= 4 && [&] { int chars = 0; for (int i = 0; i < n; ++i) chars += (int) std::strlen (spec.choices[i]); return n <= 2 || chars <= 13; }())
    {
        for (int i = 0; i < n; ++i)
        {
            auto* b = segs.add (new juce::TextButton (spec.choices[i]));
            b->setClickingTogglesState (false);
            b->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < n - 1 ? juce::Button::ConnectedOnRight : 0));
            styleSmallButton (*b);
            b->onClick = [this, i, toNorm] { if (choiceAtt) choiceAtt->setValueAsCompleteGesture (toNorm (i)); };
            addAndMakeVisible (b);
        }
        choiceAtt = std::make_unique<juce::ParameterAttachment> (*param, [this, n] (float v)
        {
            const int idx = juce::roundToInt (v * (float) (n - 1));
            for (int i = 0; i < segs.size(); ++i) segs[i]->setToggleState (i == idx, juce::dontSendNotification);
        });
    }
    else
    {
        combo = std::make_unique<juce::ComboBox>();
        for (int i = 0; i < n; ++i) combo->addItem (spec.choices[i], i + 1);
        combo->onChange = [this, toNorm] { if (choiceAtt) choiceAtt->setValueAsCompleteGesture (toNorm (combo->getSelectedItemIndex())); };
        addAndMakeVisible (*combo);
        choiceAtt = std::make_unique<juce::ParameterAttachment> (*param, [this, n] (float v)
        {
            combo->setSelectedItemIndex (juce::roundToInt (v * (float) (n - 1)), juce::dontSendNotification);
        });
    }
    choiceAtt->sendInitialUpdate();
}

ParamControl::~ParamControl()
{
    knobAtt.reset();
    choiceAtt.reset();
}

void ParamControl::resized()
{
    auto r = getLocalBounds();
    caption.setBounds (r.removeFromTop (16));
    if (knob != nullptr) { knob->setBounds (r.reduced (2, 0)); return; }
    auto mid = r.withSizeKeepingCentre (r.getWidth() - 8, 26).translated (0, -8);
    if (combo != nullptr) { combo->setBounds (mid); return; }
    const int n = segs.size();
    if (n == 0) return;
    // stack two-option toggles fit one row; more options wrap to two rows if narrow
    const int w = mid.getWidth() / n;
    for (int i = 0; i < n; ++i) segs[i]->setBounds (mid.getX() + i * w, mid.getY(), i == n - 1 ? mid.getWidth() - w * (n - 1) : w, mid.getHeight());
}

void ParamControl::paint (juce::Graphics&) {}

void ParamControl::refresh (bool assignMode)
{
    const auto* d = efx::defForId (proc.getSlotEffectId (slot));
    if (d == nullptr || k >= d->numParams) return;
    float norm[efx::kMaxParams];
    for (int i = 0; i < efx::kMaxParams; ++i) norm[i] = proc.getNorm (slot, i);
    const bool vis = efx::paramVisible (*d, k, proc.getSlotMode (slot), norm);
    if (vis != active) { active = vis; setAlpha (vis ? 1.0f : 0.32f); }

    if (knob != nullptr)
    {
        const float depth = proc.getMacroDepth (slot, k);
        auto* fx = proc.getEffect (slot);
        const float eff = (fx != nullptr && fx->getId() == proc.getSlotEffectId (slot)) ? fx->effectiveNorm (k) : (float) knob->getValue();
        auto& props = knob->getProperties();
        const bool changed = (float) props["macroDepth"] != depth || std::abs ((float) props["effNorm"] - eff) > 0.002f
                             || (bool) props["assign"] != assignMode;
        props.set ("macroDepth", depth);
        props.set ("effNorm", eff);
        props.set ("assign", assignMode);
        knob->assign = assignMode;
        knob->setMouseCursor (assignMode ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        if (changed) knob->repaint();
    }
}

// ============================================================
// GridTile
// ============================================================
GridTile::GridTile (EfxGridProcessor& p, int slotIndex) : proc (p), index (slotIndex)
{
    setRepaintsOnMouseActivity (true);
}

juce::Rectangle<float> GridTile::bypassArea() const
{
    return tileLayout (getLocalBounds().toFloat()).power;
}

juce::Rectangle<float> GridTile::expandArea() const
{
    return tileLayout (getLocalBounds().toFloat()).expand;
}

juce::Rectangle<float> GridTile::macroArea() const
{
    return tileLayout (getLocalBounds().toFloat()).macro;
}

float GridTile::macroValue() const
{
    return proc.apvts.getRawParameterValue (EfxGridProcessor::paramId (index, "macro"))->load();
}

void GridTile::setMacro (float v)
{
    if (auto* prm = proc.apvts.getParameter (EfxGridProcessor::paramId (index, "macro")))
        prm->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v));
    repaint();
}

void GridTile::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto* d = efx::defForId (proc.getSlotEffectId (index));
    if (d == nullptr || d->cat == efx::CatAnalysis || ! macroArea().contains (e.position)) { Component::mouseWheelMove (e, w); return; }
    setParam (proc, index, "macro", juce::jlimit (0.0f, 1.0f, macroValue() + (w.deltaY + w.deltaX) * (e.mods.isShiftDown() ? 0.05f : 0.2f)));
    repaint();
}

void GridTile::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    macroDrag = false;
    const int fx = proc.getSlotEffectId (index);
    const auto* d = efx::defForId (fx);
    if (d == nullptr) return;
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem (1, "Open");
        m.addItem (2, slotBypassed (proc, index) ? "Enable" : "Bypass");
        m.addItem (3, "Reset to defaults");
        m.addItem (4, "Replace effect...");
        m.addSeparator();
        m.addItem (5, "Move left", index > 0);
        m.addItem (6, "Move right", index < proc.chainLength() - 1);
        m.addSeparator();
        m.addItem (7, "Remove");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r)
        {
            switch (r)
            {
                case 1: if (onOpen) onOpen (index); break;
                case 2: setParam (proc, index, "bypass", slotBypassed (proc, index) ? 0.0f : 1.0f); break;
                case 3: proc.resetSlot (index); break;
                case 4: showEffectMenu (*this, proc.getSlotEffectId (index), true, [this] (int id)
                        {
                            if (id == 0) proc.clearSlotCompact (index); else proc.loadEffect (index, id);
                            if (onChanged) onChanged();
                        }); break;
                case 5: proc.moveSlot (index, index - 1); break;
                case 6: proc.moveSlot (index, index + 1); break;
                case 7: proc.clearSlotCompact (index); break;
                default: return;
            }
            if (onChanged) onChanged();
            repaint();
        });
        return;
    }
    if (bypassArea().expanded (4.0f).contains (e.position))
    {
        setParam (proc, index, "bypass", slotBypassed (proc, index) ? 0.0f : 1.0f);
        repaint();
        return;
    }
    if (expandArea().expanded (4.0f).contains (e.position)) return;   // opens on mouse up
    if (d->cat != efx::CatAnalysis && macroArea().contains (e.position))
    {
        // knob: drag up / right to raise (shift = fine)
        if (auto* prm = proc.apvts.getParameter (EfxGridProcessor::paramId (index, "macro"))) prm->beginChangeGesture();
        macroDrag = true;
        macroStart = macroValue();
    }
}

void GridTile::mouseDrag (const juce::MouseEvent& e)
{
    if (macroDrag)
    {
        const auto o = e.getOffsetFromDragStart().toFloat();
        setMacro (macroStart + (o.x - o.y) / (e.mods.isShiftDown() ? 800.0f : 200.0f));
        return;
    }
    if (dragging || proc.getSlotEffectId (index) == 0 || e.mods.isPopupMenu()) return;
    if (e.getDistanceFromDragStart() > 8)
        if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragging = true;
            c->startDragging ("slot:" + juce::String (index), this, juce::ScaledImage (createComponentSnapshot (getLocalBounds()).rescaled (getWidth() / 2, getHeight() / 2)));
        }
}

void GridTile::mouseUp (const juce::MouseEvent& e)
{
    if (macroDrag)
    {
        if (auto* prm = proc.apvts.getParameter (EfxGridProcessor::paramId (index, "macro"))) prm->endChangeGesture();
        macroDrag = false;
        return;
    }
    if (dragging || e.mods.isPopupMenu() || e.getDistanceFromDragStart() > 8) { dragging = false; return; }
    if (bypassArea().expanded (4.0f).contains (e.position)) return;
    // click the visual, the name or the expand button to open the full editor
    if (proc.getSlotEffectId (index) > 0 && onOpen) onOpen (index);
}

void GridTile::mouseDoubleClick (const juce::MouseEvent& e)
{
    // double-click on the macro knob returns it to zero
    if (macroArea().contains (e.position)) setParam (proc, index, "macro", 0.0f);
}

bool GridTile::isInterestedInDragSource (const SourceDetails& s)
{
    return s.description.toString().startsWith ("slot:");
}

void GridTile::itemDropped (const SourceDetails& s)
{
    dropHover = false;
    const int from = s.description.toString().fromFirstOccurrenceOf ("slot:", false, false).getIntValue();
    proc.moveSlot (from, index);
    if (onChanged) onChanged();
    repaint();
}

void GridTile::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const auto* d = efx::defForId (proc.getSlotEffectId (index));
    if (d == nullptr) return;
    const bool byp = slotBypassed (proc, index);
    const auto accent = accentForSlot (proc, index);
    const bool hot = isMouseOver (true) && ! macroDrag;

    g.setColour (PastelColors::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (dropHover ? juce::Colours::white : (selected ? accent : (hot ? accent.withAlpha (0.45f) : PastelColors::panelEdge)));
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, selected || dropHover ? 2.0f : 1.2f);

    const auto L = tileLayout (r);

    // head: dot + name ... slot number + power
    g.setColour (accent.withAlpha (byp ? 0.4f : 1.0f));
    g.fillEllipse (L.head.getX(), L.head.getCentreY() - 3.5f, 7.0f, 7.0f);
    g.setColour (byp ? PastelColors::textDim : PastelColors::text);
    g.setFont (fnt (L.head.getWidth() > 200.0f ? 14.5f : 13.0f, true));
    g.drawFittedText (d->name, L.head.withTrimmedLeft (14.0f).withTrimmedRight (66.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    g.setColour (juce::Colour (0xFF55585E));
    g.setFont (fnt (10.0f, true));
    g.drawText (slotLabel (index), L.head.withTrimmedRight (43.0f).toNearestInt(), juce::Justification::centredRight, false);
    {
        // expand: two corner arrows
        const auto b = L.expand.reduced (2.5f);
        const bool hotX = hot && L.expand.expanded (4.0f).contains (getMouseXYRelative().toFloat());
        g.setColour (hotX ? accent : PastelColors::textDim);
        juce::Path ex;
        const float k = b.getWidth() * 0.38f;
        ex.startNewSubPath (b.getRight() - k, b.getY()); ex.lineTo (b.getRight(), b.getY()); ex.lineTo (b.getRight(), b.getY() + k);
        ex.startNewSubPath (b.getX() + k, b.getBottom()); ex.lineTo (b.getX(), b.getBottom()); ex.lineTo (b.getX(), b.getBottom() - k);
        ex.startNewSubPath (b.getRight(), b.getY()); ex.lineTo (b.getCentreX() + 1.0f, b.getCentreY() - 1.0f);
        ex.startNewSubPath (b.getX(), b.getBottom()); ex.lineTo (b.getCentreX() - 1.0f, b.getCentreY() + 1.0f);
        g.strokePath (ex, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    {
        const auto b = bypassArea();
        g.setColour (byp ? juce::Colour (0xFF1A1C21) : accent.withAlpha (0.18f));
        g.fillEllipse (b);
        g.setColour (byp ? juce::Colour (0xFF55585E) : accent);
        g.drawEllipse (b.reduced (0.5f), 1.2f);
        juce::Path pw;
        pw.addCentredArc (b.getCentreX(), b.getCentreY() + 0.5f, 3.6f, 3.6f, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
        g.strokePath (pw, juce::PathStrokeType (1.2f));
        g.fillRect (b.getCentreX() - 0.6f, b.getCentreY() - 4.5f, 1.2f, 4.0f);
    }

    std::shared_ptr<const efx::Wavetable> holder;
    auto st = buildVizState (proc, index, false, holder);
    if (L.viz.getHeight() > 8.0f)
    {
        if (byp) st.accent = accent.withSaturation (0.1f).withAlpha (0.6f);
        viz::drawEffectVisual (g, L.viz, st);
        // the sequencer has this slot off right now: the tile dims, so the grid still reads as a live chain
        if (! byp && proc.seqRunning() && proc.getSlotLanes (index).mode == seq::Sequenced && ! proc.slotGateOpen (index))
        {
            g.setColour (juce::Colour (0x99141518));
            g.fillRoundedRectangle (L.viz, 6.0f);
        }
        if (byp)
        {
            g.setColour (juce::Colour (0xAA141518));
            g.fillRoundedRectangle (L.viz, 6.0f);
            g.setColour (PastelColors::textDim);
            g.setFont (fnt (11.0f, true));
            g.drawText ("BYPASSED", L.viz, juce::Justification::centred);
        }
    }

    // macro knob (meters show their readout instead)
    const auto m = L.macro;
    if (d->cat == efx::CatAnalysis)
    {
        g.setColour (juce::Colour (0xFF1A1C21));
        g.fillRoundedRectangle (m, 6.0f);
        g.setColour (PastelColors::text);
        g.setFont (fnt (juce::jlimit (12.0f, 22.0f, m.getHeight() * 0.22f), true));
        g.drawFittedText (viz::readoutTag (st), m.reduced (8.0f).toNearestInt(), juce::Justification::centred, 2, 0.8f);
        return;
    }
    const float v = macroValue();
    const auto kb = tileKnob (m);
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float av = a0 + (a1 - a0) * v;
    const float rad = kb.getWidth() * 0.5f, thick = juce::jlimit (2.5f, 7.0f, rad * 0.14f);
    {
        juce::Path track; track.addCentredArc (kb.getCentreX(), kb.getCentreY(), rad - thick, rad - thick, 0.0f, a0, a1, true);
        g.setColour (juce::Colour (0xFF24262C));
        g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (v > 0.001f)
        {
            juce::Path val; val.addCentredArc (kb.getCentreX(), kb.getCentreY(), rad - thick, rad - thick, 0.0f, a0, av, true);
            g.setColour (accent.withAlpha (byp ? 0.4f : 1.0f));
            g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        const auto inner = kb.reduced (thick * 2.4f);
        g.setColour (macroDrag || (hot && m.contains (getMouseXYRelative().toFloat())) ? juce::Colour (0xFF2A2D33) : juce::Colour (0xFF202227));
        g.fillEllipse (inner);
        // value dot on the arc, percentage in the middle
        const auto dir = juce::Point<float> (std::sin (av), -std::cos (av));
        const auto dot = kb.getCentre() + dir * (rad - thick);
        g.setColour (byp ? PastelColors::textDim : juce::Colours::white);
        g.fillEllipse (juce::Rectangle<float> (thick * 1.5f, thick * 1.5f).withCentre (dot));
        if (inner.getWidth() > 46.0f)
        {
            g.setColour (PastelColors::text);
            g.setFont (fnt (juce::jlimit (10.0f, 18.0f, inner.getWidth() * 0.22f), true));
            g.drawText (juce::String (juce::roundToInt (v * 100.0f)) + "%", inner.toNearestInt(), juce::Justification::centred, false);
        }
    }
    auto cap = juce::Rectangle<float> (m.getX(), m.getBottom() - 16.0f, m.getWidth(), 16.0f);
    g.setColour (PastelColors::textDim);
    g.setFont (fnt (10.0f, true));
    const juce::String label = proc.macroLabel (index) + (kb.getWidth() > 66.0f ? juce::String() : "  " + juce::String (juce::roundToInt (v * 100.0f)) + "%");
    g.drawFittedText (label, cap.toNearestInt(), juce::Justification::centred, 1, 0.8f);
}

// ============================================================
// AddTile
// ============================================================
void AddTile::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool hot = isMouseOver();
    g.setColour (hot ? juce::Colour (0xFF1E2025) : juce::Colour (0xFF18191D));
    g.fillRoundedRectangle (r, 10.0f);
    const float dash[] = { 5.0f, 5.0f };
    juce::Path outline; outline.addRoundedRectangle (r, 10.0f);
    juce::Path dashed; juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dash, 2);
    g.setColour (hot ? PastelColors::accent.withAlpha (0.7f) : juce::Colour (0xFF3A3D44));
    g.fillPath (dashed);

    const float d = juce::jlimit (22.0f, empty ? 84.0f : 46.0f, juce::jmin (r.getWidth(), r.getHeight()) * 0.35f);
    auto c = juce::Rectangle<float> (d, d).withCentre (r.getCentre().translated (0.0f, empty ? -16.0f : 0.0f));
    g.setColour (hot ? PastelColors::accent : PastelColors::textDim.withAlpha (0.8f));
    g.drawEllipse (c, 1.5f);
    const float a = d * 0.22f, w = juce::jmax (1.5f, d * 0.04f);
    g.fillRoundedRectangle (c.getCentreX() - a, c.getCentreY() - w * 0.5f, a * 2.0f, w, w * 0.5f);
    g.fillRoundedRectangle (c.getCentreX() - w * 0.5f, c.getCentreY() - a, w, a * 2.0f, w * 0.5f);
    if (empty)
    {
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (13.0f, true));
        g.drawText ("ADD AN EFFECT", juce::Rectangle<float> (r.getX(), c.getBottom() + 14.0f, r.getWidth(), 18.0f), juce::Justification::centred, false);
        g.setFont (fnt (11.0f));
        g.setColour (PastelColors::textDim.withAlpha (0.7f));
        g.drawText ("Effects run left to right, top to bottom", juce::Rectangle<float> (r.getX(), c.getBottom() + 34.0f, r.getWidth(), 16.0f), juce::Justification::centred, false);
    }
}

// ============================================================
// ExpandedPanel
// ============================================================
ExpandedPanel::ExpandedPanel (EfxGridProcessor& p) : proc (p), vizEditor (p)
{
    setInterceptsMouseClicks (true, true);
    setWantsKeyboardFocus (true);
    setVisible (false);
    addAndMakeVisible (vizEditor);

    pickBtn.setTooltip ("Replace the effect in this slot");
    pickBtn.onClick = [this]
    {
        showEffectMenu (pickBtn, proc.getSlotEffectId (slot), true, [this] (int id)
        {
            if (id == 0) { proc.clearSlotCompact (slot); if (onChanged) onChanged(); if (onClose) onClose(); return; }
            proc.loadEffect (slot, id);
            if (onChanged) onChanged();
        });
    };
    bypassBtn.setClickingTogglesState (false);
    bypassBtn.onClick = [this] { if (bypassAtt) bypassAtt->setValueAsCompleteGesture (slotBypassed (proc, slot) ? 0.0f : 1.0f); };
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    prevBtn.onClick = [this] { if (onNavSlot) onNavSlot (-1); };
    nextBtn.onClick = [this] { if (onNavSlot) onNavSlot (1); };
    prevBtn.setTooltip ("Previous slot"); nextBtn.setTooltip ("Next slot");
    for (auto* b : { &pickBtn, &bypassBtn, &closeBtn, &prevBtn, &nextBtn }) { styleSmallButton (*b); addAndMakeVisible (*b); }

    macroKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    macroKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
    macroKnob.setDoubleClickReturnValue (true, 0.0);
    macroKnob.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    addAndMakeVisible (macroKnob);
    for (auto* l : { &macroCaption, &mixCaption, &modeCaption })
    {
        l->setFont (fnt (10.5f, true));
        l->setColour (juce::Label::textColourId, PastelColors::textDim);
        addAndMakeVisible (*l);
    }
    macroCaption.setJustificationType (juce::Justification::centredLeft);
    mixCaption.setText ("MIX", juce::dontSendNotification);
    modeCaption.setText ("MODE", juce::dontSendNotification);

    assignBtn.setClickingTogglesState (true);
    assignBtn.setTooltip ("Assign mode: drag any knob to set how far the macro moves it (double-click clears)");
    assignBtn.onClick = [this] { assignMode = assignBtn.getToggleState(); tick(); repaint(); };
    routeResetBtn.setTooltip ("Restore this effect's default macro routing");
    routeResetBtn.onClick = [this] { proc.resetMacroRouting (slot); tick(); };
    for (auto* b : { &assignBtn, &routeResetBtn }) { styleSmallButton (*b); addAndMakeVisible (*b); }

    mixSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    mixSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    mixSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    addAndMakeVisible (mixSlider);

    tableLoadBtn.onClick = [this] { loadTable(); };
    tableResetBtn.onClick = [this] { proc.resetWavetable (slot); repaint(); };
    tableLoadBtn.setTooltip ("Load a single-cycle WAV or a multi-frame wavetable (2048-sample frames)");
    for (auto* b : { &tableLoadBtn, &tableResetBtn }) { styleSmallButton (*b); addChildComponent (*b); }
    irLoadBtn.onClick = [this] { loadIr(); };
    irClearBtn.onClick = [this] { proc.clearAudioAsset (slot); repaint(); };
    irLoadBtn.setTooltip ("Load an impulse response (WAV / AIFF / FLAC, up to 10 s); sets IR to User File");
    for (auto* b : { &irLoadBtn, &irClearBtn }) { styleSmallButton (*b); addChildComponent (*b); }

    resetBtn.onClick = [this] { proc.resetSlot (slot); refreshForEffect(); if (onChanged) onChanged(); };
    moveLBtn.onClick = [this] { proc.moveSlot (slot, slot - 1); if (onChanged) onChanged(); setSlot (juce::jmax (0, slot - 1)); };
    moveRBtn.onClick = [this] { if (slot < proc.chainLength() - 1) { proc.moveSlot (slot, slot + 1); if (onChanged) onChanged(); setSlot (slot + 1); } };
    removeBtn.onClick = [this] { proc.clearSlotCompact (slot); if (onChanged) onChanged(); if (onClose) onClose(); };
    resetBtn.setTooltip ("Reset every control of this effect to its default");
    randBtn.setTooltip ("Randomize this effect's knobs around their defaults");
    mutBtn.setTooltip ("Nudge this effect's knobs a little");
    randBtn.onClick = [this] { proc.randomizeSlot (slot, 0.8f); refreshForEffect(); if (onChanged) onChanged(); };
    mutBtn.onClick = [this] { proc.mutateSlot (slot, 0.3f); refreshForEffect(); if (onChanged) onChanged(); };
    for (auto* b : { &randBtn, &mutBtn }) { styleSmallButton (*b); addAndMakeVisible (*b); }
    for (auto* b : { &resetBtn, &moveLBtn, &moveRBtn, &removeBtn }) { styleSmallButton (*b); addAndMakeVisible (*b); }
}

ExpandedPanel::~ExpandedPanel()
{
    controls.clear();
    macroAtt.reset(); mixAtt.reset(); bypassAtt.reset();
}

void ExpandedPanel::setSlot (int s)
{
    slot = juce::jlimit (0, EfxGridProcessor::kSlots - 1, s);
    macroAtt.reset(); mixAtt.reset(); bypassAtt.reset();
    macroAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, EfxGridProcessor::paramId (slot, "macro"), macroKnob);
    mixAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, EfxGridProcessor::paramId (slot, "mix"), mixSlider);
    for (auto* sl : { &macroKnob, &mixSlider })
    {
        sl->textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        sl->valueFromTextFunction = [] (const juce::String& t) { return juce::jlimit (0.0, 1.0, t.retainCharacters ("0123456789.").getDoubleValue() / 100.0); };
        sl->updateText();
    }
    if (auto* bp = proc.apvts.getParameter (EfxGridProcessor::paramId (slot, "bypass")))
    {
        bypassAtt = std::make_unique<juce::ParameterAttachment> (*bp, [this] (float v)
        {
            bypassBtn.setButtonText (v > 0.5f ? "BYPASSED" : "ON");
            bypassBtn.setToggleState (v <= 0.5f, juce::dontSendNotification);
        });
        bypassAtt->sendInitialUpdate();
    }
    vizEditor.setSlot (slot);
    shownFx = -1;
    refreshForEffect();
}

void ExpandedPanel::refreshForEffect()
{
    const int fxId = proc.getSlotEffectId (slot);
    shownFx = fxId;
    const auto* d = efx::defForId (fxId);
    getProperties().set ("accent", (int) accentForSlot (proc, slot).getARGB());

    controls.clear();
    modeBtns.clear();
    if (d != nullptr)
    {
        for (int k = 0; k < d->numParams; ++k)
        {
            auto* c = controls.add (new ParamControl (proc, slot, k));
            c->onRoutingChanged = [this] { tick(); };
            addAndMakeVisible (c);
        }
        for (auto* c : controls) c->sendLookAndFeelChange();   // created after the editor's LookAndFeel was set
        for (int m = 0; m < d->numModes; ++m)
        {
            auto* b = modeBtns.add (new juce::TextButton (d->modes[m]));
            b->setConnectedEdges ((m > 0 ? juce::Button::ConnectedOnLeft : 0) | (m < d->numModes - 1 ? juce::Button::ConnectedOnRight : 0));
            styleSmallButton (*b);
            b->onClick = [this, m] { proc.setSlotMode (slot, m); updateModeButtons(); };
            addAndMakeVisible (b);
        }
        pickBtn.setButtonText (juce::String (d->name) + "  v");
        mixSlider.setDoubleClickReturnValue (true, d->mixDef);
    }
    else pickBtn.setButtonText ("Choose effect  v");

    const bool meter = d != nullptr && d->cat == efx::CatAnalysis;
    const bool hasMacro = d != nullptr && ! meter;
    for (auto* c : std::initializer_list<juce::Component*> { &macroKnob, &macroCaption, &assignBtn, &routeResetBtn }) c->setVisible (hasMacro);
    mixSlider.setVisible (hasMacro); mixCaption.setVisible (hasMacro);
    modeCaption.setVisible (d != nullptr && d->numModes > 1);
    for (auto* b : modeBtns) b->setVisible (d != nullptr && d->numModes > 1);
    const bool wt = d != nullptr && d->usesWavetable();
    tableLoadBtn.setVisible (wt); tableResetBtn.setVisible (wt);
    const bool ir = d != nullptr && d->viz == efx::VizConv;
    irLoadBtn.setVisible (ir); irClearBtn.setVisible (ir);
    if (! hasMacro && assignMode) { assignMode = false; assignBtn.setToggleState (false, juce::dontSendNotification); }
    updateModeButtons();
    layoutPanel();
    tick();
    repaint();
}

void ExpandedPanel::updateModeButtons()
{
    const int m = proc.getSlotMode (slot);
    for (int i = 0; i < modeBtns.size(); ++i) modeBtns[i]->setToggleState (i == m, juce::dontSendNotification);
}

void ExpandedPanel::loadTable()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a wavetable", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        juce::String err;
        if (! proc.loadWavetable (slot, f, err))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Wavetable", err);
        repaint();
    });
}

void ExpandedPanel::loadIr()
{
    chooser = std::make_unique<juce::FileChooser> ("Load an impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        juce::String err;
        if (! proc.loadAudioAsset (slot, f, err))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Impulse response", err);
            return;
        }
        // switch the IR selector to the user file
        if (const auto* d = efx::defForId (proc.getSlotEffectId (slot)))
            for (int k = 0; k < d->numParams; ++k)
                if (juce::String (d->p[k].name) == "IR" && d->p[k].isChoice())
                    setParam (proc, slot, ("p" + juce::String (k)).toRawUTF8(), 1.0f);
        proc.undoCheckpoint (true);   // the selector switch belongs to the load step
        repaint();
    });
}

void ExpandedPanel::tick()
{
    if (proc.getSlotEffectId (slot) != shownFx) { refreshForEffect(); return; }
    for (auto* c : controls) c->refresh (assignMode);
    macroCaption.setText ("MACRO  -  " + proc.macroLabel (slot).toUpperCase(), juce::dontSendNotification);
    updateModeButtons();
    vizEditor.repaint();
    repaint (meterArea.expanded (2).getUnion (routeArea).getUnion (tableArea).getUnion (scArea));
}

void ExpandedPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! panelArea.contains (e.getPosition()) && onClose) onClose();
}

bool ExpandedPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
    if (k == juce::KeyPress::leftKey && k.getModifiers().isCommandDown()) { if (onNavSlot) onNavSlot (-1); return true; }
    if (k == juce::KeyPress::rightKey && k.getModifiers().isCommandDown()) { if (onNavSlot) onNavSlot (1); return true; }
    return false;
}

void ExpandedPanel::layoutPanel()
{
    panelArea = getLocalBounds().reduced (16).withSizeKeepingCentre (juce::jmin (1200, getWidth() - 32), juce::jmin (900, getHeight() - 32));
    auto d = panelArea.reduced (20, 14);

    // header: [dot CAT / name] ... [<][>] [picker] [bypass] [x]
    headArea = d.removeFromTop (46);
    auto h = headArea;
    closeBtn.setBounds (h.removeFromRight (30).withSizeKeepingCentre (30, 28));
    h.removeFromRight (8);
    bypassBtn.setBounds (h.removeFromRight (96).withSizeKeepingCentre (96, 28));
    h.removeFromRight (8);
    pickBtn.setBounds (h.removeFromRight (200).withSizeKeepingCentre (200, 28));
    h.removeFromRight (12);
    nextBtn.setBounds (h.removeFromRight (28).withSizeKeepingCentre (28, 28));
    h.removeFromRight (4);
    prevBtn.setBounds (h.removeFromRight (28).withSizeKeepingCentre (28, 28));
    d.removeFromTop (12);

    // right column
    rightCol = d.removeFromRight (220);
    d.removeFromRight (16);
    auto rc = rightCol;
    macroCaption.setBounds (rc.removeFromTop (18));
    macroKnob.setBounds (rc.removeFromTop (112).withSizeKeepingCentre (112, 112));
    rc.removeFromTop (6);
    {
        auto row = rc.removeFromTop (26);
        const int w = (row.getWidth() - 6) / 2;
        assignBtn.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        routeResetBtn.setBounds (row);
    }
    rc.removeFromTop (12);
    mixCaption.setBounds (rc.removeFromTop (14));
    mixSlider.setBounds (rc.removeFromTop (24));
    rc.removeFromTop (10);
    if (modeBtns.size() > 1)
    {
        modeCaption.setBounds (rc.removeFromTop (14));
        auto row = rc.removeFromTop (28);
        const int w = row.getWidth() / modeBtns.size();
        for (int i = 0; i < modeBtns.size(); ++i)
            modeBtns[i]->setBounds (row.getX() + i * w, row.getY(), i == modeBtns.size() - 1 ? row.getWidth() - w * i : w, row.getHeight());
        rc.removeFromTop (10);
    }
    tableArea = {};
    auto fileRow = [&] (juce::TextButton& a, juce::TextButton& b)
    {
        tableArea = rc.removeFromTop (18);
        auto row = rc.removeFromTop (26);
        const int w = (row.getWidth() - 6) * 3 / 5;
        a.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        b.setBounds (row);
        rc.removeFromTop (10);
    };
    if (tableLoadBtn.isVisible()) fileRow (tableLoadBtn, tableResetBtn);
    else if (irLoadBtn.isVisible()) fileRow (irLoadBtn, irClearBtn);
    scArea = {};
    if (const auto* fd = efx::defForId (proc.getSlotEffectId (slot)); fd != nullptr && fd->usesSidechain())
    {
        scArea = rc.removeFromTop (18);
        rc.removeFromTop (8);
    }
    auto bottom = rightCol.removeFromBottom (96);
    {
        auto r0 = bottom.removeFromTop (28);
        const int w0 = (r0.getWidth() - 6) / 2;
        randBtn.setBounds (r0.removeFromLeft (w0)); r0.removeFromLeft (6); mutBtn.setBounds (r0);
        bottom.removeFromTop (6);
    }
    routeArea = rc.withBottom (bottom.getY() - 10);
    {
        auto r1 = bottom.removeFromTop (28);
        const int w = (r1.getWidth() - 6) / 2;
        moveLBtn.setBounds (r1.removeFromLeft (w)); r1.removeFromLeft (6); moveRBtn.setBounds (r1);
        bottom.removeFromTop (6);
        auto r2 = bottom.removeFromTop (28);
        resetBtn.setBounds (r2.removeFromLeft (w)); r2.removeFromLeft (6); removeBtn.setBounds (r2);
    }

    // controls: up to six in one row, otherwise two (or more) rows
    hintArea = d.removeFromBottom (18);
    d.removeFromBottom (4);
    const int n = juce::jmax (1, controls.size());
    // rows of control indices: some effects group theirs (one EQ band per
    // row); otherwise fill evenly, adding rows when the window is too narrow
    // for ~84 px per control. Rows shrink so the visual keeps ~170 px.
    std::vector<std::vector<int>> rowsK;
    bool grouped = false;   // grouped rows line up in columns instead of centring
    const auto* def = efx::defForId (proc.getSlotEffectId (slot));
    if (def != nullptr && def->viz == efx::VizEq3 && controls.size() == 13)
    {
        rowsK = { { 0, 3, 11, 8 }, { 1, 4, 6, 9 }, { 2, 5, 12, 10, 7 } };
        grouped = true;
    }
    else if (def != nullptr && def->viz == efx::VizTransient && controls.size() == 7)
        rowsK = { { 0, 6, 1, 5 }, { 2, 3, 4 } };   // envelope order: Attack Decay Sustain Release
    else
    {
        const int minCw = 84;
        const int nr = juce::jmax (n > 6 ? 2 : 1, (n * minCw + d.getWidth() - 1) / juce::jmax (1, d.getWidth()));
        const int per = (n + nr - 1) / nr;
        for (int k = 0; k < controls.size(); ++k)
        {
            if (k % per == 0) rowsK.emplace_back();
            rowsK.back().push_back (k);
        }
    }
    const int rows = juce::jmax (1, (int) rowsK.size());
    int perRow = 1;
    for (auto& rk : rowsK) perRow = juce::jmax (perRow, (int) rk.size());
    const int rowH = juce::jlimit (78, 104, (d.getHeight() - 14 - 170) / rows);
    controlsArea = d.removeFromBottom (rows * rowH);
    d.removeFromBottom (14);
    vizArea = d;
    meterArea = vizArea.removeFromRight (18);
    vizArea.removeFromRight (6);
    vizEditor.setBounds (vizArea.reduced (2));

    const int cw = juce::jmin (130, controlsArea.getWidth() / juce::jmax (1, perRow));
    for (int r = 0; r < (int) rowsK.size(); ++r)
    {
        const int rowCount = (int) rowsK[(size_t) r].size();
        const int x0 = controlsArea.getX() + (controlsArea.getWidth() - (grouped ? perRow : rowCount) * cw) / 2;   // centre short rows
        for (int c = 0; c < rowCount; ++c)
            controls[rowsK[(size_t) r][(size_t) c]]->setBounds (x0 + c * cw, controlsArea.getY() + r * rowH, cw, rowH - 6);
    }
}

void ExpandedPanel::resized()
{
    layoutPanel();
}

void ExpandedPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xE00B0C0E));

    auto p = panelArea.toFloat();
    g.setColour (PastelColors::panel);
    g.fillRoundedRectangle (p, 12.0f);
    g.setColour (PastelColors::panelEdge);
    g.drawRoundedRectangle (p.reduced (0.5f), 12.0f, 1.5f);

    const int fxId = proc.getSlotEffectId (slot);
    const auto* d = efx::defForId (fxId);
    const auto accent = accentForSlot (proc, slot);
    auto* fx = proc.getEffect (slot);
    if (fx != nullptr && fx->getId() != fxId) fx = nullptr;

    // header
    {
        auto head = headArea.toFloat();
        g.setColour (accent);
        g.fillEllipse (head.getX(), head.getY() + 7.0f, 11.0f, 11.0f);
        const float x = head.getX() + 19.0f;
        const juce::String cat = d ? juce::String (efx::catName ((int) d->cat)).toUpperCase() : "EMPTY";
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.5f, true));
        g.drawText (cat + "  /  SLOT " + slotLabel (slot), juce::Rectangle<float> (x, head.getY() + 2.0f, 260.0f, 20.0f), juce::Justification::centredLeft, false);
        g.setColour (PastelColors::text);
        g.setFont (fnt (19.0f, true));
        const float textW = (float) (prevBtn.getX() - (int) x - 10);
        g.drawFittedText (d ? juce::String (d->name) : juce::String ("Empty slot"), juce::Rectangle<float> (x, head.getY() + 18.0f, textW, 26.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.8f);
    }
    g.setColour (PastelColors::panelEdge);
    g.fillRect ((float) panelArea.getX() + 22.0f, (float) headArea.getBottom() + 5.0f, (float) panelArea.getWidth() - 44.0f, 1.0f);

    // visualiser background (the VizEditor child draws the visual itself)
    g.setColour (juce::Colour (0xFF1A1C21));
    g.fillRoundedRectangle (vizArea.toFloat(), 8.0f);
    g.setColour (PastelColors::panelEdge);
    g.drawRoundedRectangle (vizArea.toFloat().reduced (0.5f), 8.0f, 1.2f);

    // level meter
    {
        const float lv = fx ? juce::jlimit (0.0f, 1.0f, (fx->peakDb() + 60.0f) / 60.0f) : 0.0f;
        const int segs = 12;
        const float sh = (float) meterArea.getHeight() / (float) segs;
        for (int i = 0; i < segs; ++i)
        {
            const bool lit = (float) (i + 1) / (float) segs <= lv;
            const auto c = i >= segs - 1 ? juce::Colour (0xFFE88A8A) : accent;
            g.setColour (lit ? c.withAlpha (0.9f) : juce::Colour (0xFF2A2D34).withAlpha (0.6f));
            g.fillRoundedRectangle ((float) meterArea.getX(), (float) meterArea.getY() + (float) (segs - 1 - i) * sh, (float) meterArea.getWidth(), sh - 3.0f, 2.5f);
        }
    }

    // description + how to edit the visual
    if (d != nullptr)
    {
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.5f));
        const auto hint = vizEditor.hint();
        g.drawText (hint.isEmpty() ? juce::String (d->desc) : juce::String (d->desc) + "     |     " + hint,
                    hintArea, juce::Justification::centredLeft, true);
    }

    // wavetable / IR name
    if (! tableArea.isEmpty() && irLoadBtn.isVisible())
    {
        const auto a = proc.getAudioAsset (slot);
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.5f, true));
        g.drawFittedText ("USER IR  -  " + (a != nullptr ? a->name.toUpperCase() + "  (" + juce::String (a->buf.getNumSamples() / a->sampleRate, 1) + " s)" : juce::String ("NONE LOADED")),
                          tableArea, juce::Justification::centredLeft, 1, 0.8f);
    }
    else if (! tableArea.isEmpty())
    {
        const auto t = proc.getWavetable (slot);
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.5f, true));
        g.drawFittedText ("TABLE  -  " + t->name.toUpperCase() + "  (" + juce::String (t->numFrames) + (t->numFrames == 1 ? " frame)" : " frames)"),
                          tableArea, juce::Justification::centredLeft, 1, 0.8f);
    }
    // sidechain status for effects that can key from it
    if (d != nullptr && d->usesSidechain())
    {
        const bool ext = d->modes != nullptr && juce::String (d->modes[proc.getSlotMode (slot)]) == "Ext";
        const bool sc = proc.sidechainAvailable();
        g.setColour (sc ? accent : PastelColors::textDim);
        g.fillEllipse ((float) scArea.getX(), (float) scArea.getCentreY() - 3.5f, 7.0f, 7.0f);
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.0f, true));
        g.drawText (sc ? (ext ? "SIDECHAIN IN USE" : "SIDECHAIN CONNECTED (select Ext)") : (ext ? "EXT: NO SIDECHAIN - USING INPUT" : "SIDECHAIN NOT CONNECTED"),
                    scArea.withTrimmedLeft (14), juce::Justification::centredLeft, false);
    }
    // macro routing list
    if (d != nullptr && d->cat != efx::CatAnalysis && routeArea.getHeight() > 30)
    {
        auto ra = routeArea;
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.5f, true));
        g.drawText ("MACRO ROUTING", ra.removeFromTop (16), juce::Justification::centredLeft, false);
        ra.removeFromTop (2);
        int shown = 0;
        for (int k = 0; k < d->numParams && ra.getHeight() >= 17; ++k)
        {
            const float dep = proc.getMacroDepth (slot, k);
            if (std::abs (dep) < 0.005f) continue;
            auto row = ra.removeFromTop (17);
            g.setColour (PastelColors::text);
            g.setFont (fnt (11.0f));
            g.drawText (d->p[k].name, row.removeFromLeft (84), juce::Justification::centredLeft, true);
            g.setColour (PastelColors::textDim);
            g.drawText ((dep > 0 ? "+" : "") + juce::String (juce::roundToInt (dep * 100.0f)) + "%", row.removeFromRight (40), juce::Justification::centredRight, false);
            auto bar = row.reduced (4, 6).toFloat();
            g.setColour (PastelColors::track);
            g.fillRoundedRectangle (bar, 2.0f);
            const float cx = bar.getCentreX(), w = bar.getWidth() * 0.5f * juce::jlimit (-1.0f, 1.0f, dep);
            g.setColour (dep > 0 ? accent : juce::Colour (0xFFE88A8A));
            g.fillRect (juce::jmin (cx, cx + w), bar.getY(), std::abs (w), bar.getHeight());
            ++shown;
        }
        if (shown == 0)
        {
            g.setColour (PastelColors::textDim.withAlpha (0.8f));
            g.setFont (fnt (10.5f));
            g.drawFittedText ("Nothing assigned. Press ASSIGN and drag knobs, or right-click a knob.", ra.removeFromTop (34), juce::Justification::topLeft, 2);
        }
    }
    if (assignMode)
    {
        g.setColour (accent);
        g.setFont (fnt (10.5f, true));
        g.drawText ("ASSIGN MODE  -  drag a knob to set its macro depth, double-click to clear",
                    juce::Rectangle<int> (controlsArea.getX(), controlsArea.getY() - 14, controlsArea.getWidth(), 14), juce::Justification::centred, false);
    }
}

// ============================================================
// EfxGridEditor
// ============================================================
EfxGridEditor::EfxGridEditor (EfxGridProcessor& p) : juce::AudioProcessorEditor (p), undoSteps (p), proc (p), overlay (p), midiBtn (p.midiLearn), browser (p), seqView (p)
{
    lnf = std::make_unique<PastelLookAndFeel>();
    setLookAndFeel (lnf.get());
    setOpaque (true);

    for (int i = 0; i < EfxGridProcessor::kSlots; ++i)
    {
        auto t = std::make_unique<GridTile> (proc, i);
        t->onOpen = [this] (int s) { openSlot (s); };
        t->onChanged = [this] { refreshFromModel(); };
        addChildComponent (*t);
        tiles[(size_t) i] = std::move (t);
    }
    addTile.onClick = [this] { addEffectMenu (addTile); };
    addAndMakeVisible (addTile);

    overlay.onClose = [this] { closeOverlay(); };
    overlay.onChanged = [this] { refreshFromModel(); };
    overlay.onNavSlot = [this] (int dlt)
    {
        const int n = proc.chainLength();
        if (n <= 0) return;
        const int s = ((overlay.shownSlot() + dlt) % n + n) % n;
        openSlot (s);
    };
    addChildComponent (overlay);

    proc.onSlotsChanged = [this] { refreshFromModel(); };

    // presets: < name > in the header; the name opens the browser
    presetBtn.setTooltip ("Presets: browse, load and save whole chains");
    presetBtn.onClick = [this] { browser.setVisible (true); browser.toFront (true); };
    presetPrev.onClick = [this] { presets::step (proc, -1); presetChanged(); };
    presetNext.onClick = [this] { presets::step (proc, 1); presetChanged(); };
    presetPrev.setTooltip ("Previous preset"); presetNext.setTooltip ("Next preset");
    midiBtn.setColour (juce::TextButton::buttonOnColourId, PastelColors::accent);   // the LookAndFeel fills toggled buttons with accentFor()
    randAllBtn.setTooltip ("Randomize every effect's knobs around its defaults. Right-click: a random new chain");
    randAllBtn.onClick = [this]
    {
        if (juce::ModifierKeys::currentModifiers.isPopupMenu()) return;
        proc.randomizeChain (0.6f); presetChanged();
    };
    randAllBtn.addMouseListener (this, false);
    mutAllBtn.setTooltip ("Nudge every effect's knobs a little from where they are");
    mutAllBtn.onClick = [this] { proc.mutateChain (0.3f); presetChanged(); };
    for (auto* b : { &presetPrev, &presetNext, &presetBtn, &randAllBtn, &mutAllBtn, static_cast<juce::TextButton*> (&midiBtn) }) { styleSmallButton (*b); addAndMakeVisible (*b); }
    gridTab.setClickingTogglesState (false); seqTab.setClickingTogglesState (false);
    gridTab.setConnectedEdges (juce::Button::ConnectedOnRight); seqTab.setConnectedEdges (juce::Button::ConnectedOnLeft);
    gridTab.onClick = [this] { showSequencer (false); };
    seqTab.onClick = [this] { showSequencer (true); };
    gridTab.setTooltip ("The effect chain"); seqTab.setTooltip ("When each effect plays: a step sequencer synced to the host");
    for (auto* b : { &gridTab, &seqTab }) { styleSmallButton (*b); addAndMakeVisible (*b); }
    addChildComponent (seqView);
    gridTab.setToggleState (true, juce::dontSendNotification);
    browser.onClose = [this] { browser.setVisible (false); grabKeyboardFocus(); };
    presetBtn.setButtonText (proc.presetName); shownPreset = proc.presetName;
    browser.onPresetChanged = [this] { presetChanged(); };
    addChildComponent (browser);
    sendLookAndFeelChange();   // children added after setLookAndFeel (the overlay) need it too

    setSize (1180, 860);
    setResizable (true, true);
    setResizeLimits (860, 640, 1800, 1400);
    setWantsKeyboardFocus (true);
    addMouseListener (&undoSteps, true);
    startTimerHz (30);
    refreshFromModel();
}

EfxGridEditor::~EfxGridEditor()
{
    removeMouseListener (&undoSteps);
    proc.onSlotsChanged = nullptr;
    setLookAndFeel (nullptr);
}

void EfxGridEditor::addEffectMenu (juce::Component& target)
{
    if (proc.firstEmptySlot() < 0) return;
    showEffectMenu (target, 0, false, [this] (int id)
    {
        if (proc.appendEffect (id)) refreshFromModel();   // stays on the grid; click the tile to open it
    });
}

void EfxGridEditor::openSlot (int slot)
{
    if (slot < 0) return;
    overlay.setSlot (slot);
    overlay.setVisible (true);
    overlay.toFront (true);
    overlay.grabKeyboardFocus();
    overlayOpen = true;
    for (int i = 0; i < EfxGridProcessor::kSlots; ++i) tiles[(size_t) i]->setSelected (i == slot);
    repaint();
}

void EfxGridEditor::closeOverlay()
{
    overlay.setVisible (false);
    overlayOpen = false;
    for (auto& t : tiles) t->setSelected (false);
    refreshFromModel();
}

void EfxGridEditor::refreshFromModel()
{
    for (int s = 0; s < EfxGridProcessor::kSlots; ++s) { shownFx[s] = proc.getSlotEffectId (s); tiles[(size_t) s]->refreshForEffect(); }
    if (shownLen != proc.chainLength()) layoutBoard();
    if (showSeq) seqView.refresh();
    if (overlayOpen)
    {
        if (proc.getSlotEffectId (overlay.shownSlot()) == 0) closeOverlay();   // e.g. undo removed it
        else overlay.refreshForEffect();
    }
    repaint();
}

void EfxGridEditor::timerCallback()
{
    if (proc.presetName != shownPreset)
    {
        shownPreset = proc.presetName;
        presetBtn.setButtonText (shownPreset);
    }
    // commit finished edits (menus, combo boxes) while the user is working;
    // otherwise adopt outside changes (automation) without recording them
    if (! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        if (undoSteps.touchedRecently()) proc.undoCheckpoint();
        else proc.undoBaseline();
    }

    bool changed = proc.stateGeneration() != shownGen;
    shownGen = proc.stateGeneration();
    for (int s = 0; s < EfxGridProcessor::kSlots; ++s) if (proc.getSlotEffectId (s) != shownFx[s]) changed = true;
    if (changed) refreshFromModel();

    if (! overlayOpen) for (auto& t : tiles) if (t->isVisible()) t->repaint();
    if (overlayOpen) overlay.tick();
}

void EfxGridEditor::presetChanged()
{
    refreshFromModel();
    if (overlayOpen) overlay.refreshForEffect();
    shownPreset = proc.presetName;
    presetBtn.setButtonText (shownPreset);
}

void EfxGridEditor::mouseDown (const juce::MouseEvent& e)
{
    // RANDOM's right-click menu
    if (e.eventComponent == &randAllBtn && e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem (1, "Randomize knobs (whole chain)");
        m.addItem (2, "Random new chain");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&randAllBtn), [this] (int r)
        {
            if (r == 1) proc.randomizeChain (0.6f);
            else if (r == 2) proc.randomNewChain();
            else return;
            presetChanged();
        });
    }
}

void EfxGridEditor::resized()
{
    auto area = getLocalBounds();
    headerArea = area.removeFromTop (44);
    {
        auto h = headerArea.reduced (20, 8);
        h.removeFromRight (60);   // chain count
        mutAllBtn.setBounds (h.removeFromRight (78)); h.removeFromRight (6);
        randAllBtn.setBounds (h.removeFromRight (78)); h.removeFromRight (6);
        midiBtn.setBounds (h.removeFromRight (60));
        // GRID | SEQUENCER after the title, the preset bar centred in what is left
        auto tabs = juce::Rectangle<int> (200, h.getY(), juce::jmin (200, juce::jmax (140, h.getRight() - 200 - 300)), h.getHeight());
        gridTab.setBounds (tabs.withWidth (tabs.getWidth() * 2 / 5));
        seqTab.setBounds (tabs.withLeft (gridTab.getRight()));
        const int barL = tabs.getRight() + 12, barR = midiBtn.getX() - 12;
        auto bar = juce::Rectangle<int> (barL, h.getY(), juce::jmax (120, barR - barL), 28)
                       .withSizeKeepingCentre (juce::jmin (320, juce::jmax (120, barR - barL)), 28);
        presetPrev.setBounds (bar.removeFromLeft (28)); bar.removeFromLeft (4);
        presetNext.setBounds (bar.removeFromRight (28)); bar.removeFromRight (4);
        presetBtn.setBounds (bar);
    }
    browser.setBounds (getLocalBounds());
    boardArea = area.reduced (18, 16);
    seqView.setBounds (area.reduced (18, 12));
    layoutBoard();
    overlay.setBounds (getLocalBounds());
}

// The chain fills the board: n effects plus the "+" cell, four per row, each
// row split equally between its cells. A "+" left alone on a new row is a
// slim strip so the effects keep the room.
void EfxGridEditor::layoutBoard()
{
    const int n = proc.chainLength();
    shownLen = n;
    const bool hasAdd = n < EfxGridProcessor::kSlots;
    addTile.empty = n == 0;
    addTile.setVisible (hasAdd && ! showSeq);
    for (int s = 0; s < EfxGridProcessor::kSlots; ++s) tiles[(size_t) s]->setVisible (s < n && ! showSeq);
    if (showSeq) return;   // the sequencer tab has the board

    if (n == 0)
    {
        addTile.setBounds (boardArea.withSizeKeepingCentre (juce::jmin (boardArea.getWidth(), 520), juce::jmin (boardArea.getHeight(), 340)));
        return;
    }
    // every tile is the same 3:2 landscape card, as large as fits four to a
    // row; the block is centred. A "+" that would start a row on its own is a
    // slim strip under the block instead of a full card.
    const int cells = n + (hasAdd ? 1 : 0);
    const bool slimAdd = hasAdd && n % 4 == 0;
    const int cardCells = slimAdd ? n : cells;
    const int cols = juce::jmin (4, cardCells), rows = (cardCells + 3) / 4;
    const int gapX = 24, gapY = 14, slimH = 54;
    const int availW = boardArea.getWidth() - gapX * (cols - 1);
    const int availH = boardArea.getHeight() - gapY * (rows - 1) - (slimAdd ? slimH + gapY : 0);
    const int w = juce::jmin (availW / cols, (int) ((float) (availH / rows) * 1.5f));
    const int h = (int) std::round ((float) w / 1.5f);
    const int blockW = cols * w + gapX * (cols - 1);
    const int blockH = rows * h + gapY * (rows - 1) + (slimAdd ? slimH + gapY : 0);
    const int x0 = boardArea.getX() + (boardArea.getWidth() - blockW) / 2;
    const int y0 = boardArea.getY() + (boardArea.getHeight() - blockH) / 2;
    for (int cell = 0; cell < cardCells; ++cell)
    {
        const juce::Rectangle<int> b (x0 + (cell % 4) * (w + gapX), y0 + (cell / 4) * (h + gapY), w, h);
        if (cell < n) tiles[(size_t) cell]->setBounds (b);
        else addTile.setBounds (b);
    }
    if (slimAdd) addTile.setBounds (x0, y0 + rows * (h + gapY), blockW, slimH);
}

void EfxGridEditor::paint (juce::Graphics& g)
{
    g.fillAll (PastelColors::bg);

    // header: logo, name, chain length, sidechain status
    {
        const float cy = (float) headerArea.getCentreY();
        for (int i = 0; i < 4; ++i)
        {
            g.setColour (efx::catAccent (i * 3 + 1));
            g.fillRoundedRectangle (20.0f + (float) (i % 2) * 10.0f, cy - 9.0f + (float) (i / 2) * 10.0f, 8.0f, 8.0f, 2.0f);
        }
        g.setColour (PastelColors::text);
        g.setFont (fnt (16.0f, true));
        g.drawText ("PASTEL  EFX  GRID", 46, headerArea.getY(), 240, headerArea.getHeight(), juce::Justification::centredLeft, false);
        g.setColour (PastelColors::textDim);
        g.setFont (fnt (10.5f, true));
        juce::String right = juce::String (proc.chainLength()) + " / 16";
        if (proc.sidechainAvailable()) right = "SIDECHAIN IN    " + right;
        g.drawText (right, headerArea.reduced (20, 0), juce::Justification::centredRight, false);
        g.setColour (PastelColors::panelEdge);
        g.fillRect (headerArea.withTop (headerArea.getBottom() - 1).reduced (18, 0));
    }

    // signal-flow chevrons between neighbouring cells of a row, and a return
    // arrow at the end of each full row (the grid tab only)
    if (showSeq) return;
    const int n = proc.chainLength();
    std::vector<juce::Rectangle<int>> cells;
    for (int s = 0; s < n; ++s) cells.push_back (tiles[(size_t) s]->getBounds());
    if (addTile.isVisible() && n > 0) cells.push_back (addTile.getBounds());
    g.setColour (PastelColors::textDim.withAlpha (0.55f));
    for (size_t i = 0; i + 1 < cells.size(); ++i)
    {
        const auto a = cells[i], b = cells[i + 1];
        if (a.getY() == b.getY())
        {
            const float cx = (float) (a.getRight() + b.getX()) * 0.5f, cy = (float) a.getCentreY();
            juce::Path p;
            p.startNewSubPath (cx - 3.0f, cy - 5.0f); p.lineTo (cx + 3.0f, cy); p.lineTo (cx - 3.0f, cy + 5.0f);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else
        {
            // wrap: small down-left hook under the row's last cell
            const float x = (float) a.getCentreX(), y0 = (float) a.getBottom() + 2.0f, y1 = (float) b.getY() - 2.0f;
            juce::Path p;
            p.startNewSubPath (x - 5.0f, y0 + 1.0f); p.lineTo (x, (y0 + y1) * 0.5f + 2.0f); p.lineTo (x + 5.0f, y0 + 1.0f);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
}

// ============================================================
juce::AudioProcessorEditor* EfxGridProcessor::createEditor()
{
    return new EfxGridEditor (*this);
}

bool EfxGridEditor::keyPressed (const juce::KeyPress& k)
{
    // undo / redo: knobs, shapes, effects added / moved / removed, files loaded
    if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z' || k.getKeyCode() == 'Y' || k.getKeyCode() == 'y'))
    {
        const bool redo = k.getModifiers().isShiftDown() || k.getKeyCode() == 'Y' || k.getKeyCode() == 'y';
        if (redo) proc.redoStep(); else proc.undoStep();
        undoSteps.touch();
        refreshFromModel();
        repaint();
        return true;
    }
    return false;
}

void EfxGridEditor::showSequencer (bool on)
{
    showSeq = on;
    gridTab.setToggleState (! on, juce::dontSendNotification);
    seqTab.setToggleState (on, juce::dontSendNotification);
    if (on && overlayOpen) closeOverlay();
    seqView.setVisible (on);
    if (on) { seqView.refresh(); seqView.toFront (false); }
    layoutBoard();
    repaint();
}
