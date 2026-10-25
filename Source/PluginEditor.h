#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <functional>
#include <memory>
#include "PluginProcessor.h"
#include "EffectRegistry.h"
#include "Theme.h"
#include "VizEditor.h"
#include "PresetBrowser.h"

class EfxGridEditor;

// categorised effect picker (popup menu with one submenu per category)
void showEffectMenu (juce::Component& target, int currentId, bool allowClear, std::function<void (int)> onPick);

// ============================================================
// One control of the slot shown in the expanded panel: a rotary knob for
// continuous values, segmented buttons for small choice sets, a combo box for
// long ones. Knobs also show and edit the slot's macro routing.
// ============================================================
class ParamControl : public juce::Component
{
public:
    ParamControl (EfxGridProcessor& p, int slot, int index);
    ~ParamControl() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh (bool assignMode);           // visibility rule, macro depth, live value
    int index() const { return k; }
    bool isKnob() const { return knob != nullptr; }

    std::function<void()> onRoutingChanged;

private:
    class Knob;
    EfxGridProcessor& proc;
    int slot, k;
    const efx::ParamSpec& spec;
    juce::Label caption;
    std::unique_ptr<Knob> knob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> knobAtt;
    juce::OwnedArray<juce::TextButton> segs;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::ParameterAttachment> choiceAtt;
    bool active = true;
};

// ============================================================
// One grid tile (a loaded slot): name, bypass, a live visualiser and a macro
// bar you can drag to play the slot's macro. Click opens, drag reorders.
// ============================================================
class GridTile : public juce::Component,
                 public juce::DragAndDropTarget
{
public:
    GridTile (EfxGridProcessor& p, int slotIndex);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails&) override;

    void setSelected (bool s) { selected = s; repaint(); }
    int slot() const { return index; }
    void refreshForEffect() { repaint(); }

    std::function<void (int)> onOpen;
    std::function<void()> onChanged;

    // tile regions (local coordinates)
    juce::Rectangle<float> bypassArea() const;
    juce::Rectangle<float> expandArea() const;
    juce::Rectangle<float> macroArea() const;

private:
    float macroValue() const;
    void setMacro (float v);
    EfxGridProcessor& proc;
    int index;
    bool selected = false, dropHover = false, dragging = false, macroDrag = false;
    float macroStart = 0.0f;
};

// ============================================================
// The "+" cell that always follows the chain (or fills the empty grid).
// ============================================================
class AddTile : public juce::Component
{
public:
    AddTile() { setRepaintsOnMouseActivity (true); }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu() && onClick) onClick(); }
    bool empty = false;          // nothing loaded yet: big centred call to action
    std::function<void()> onClick;
};

// ============================================================
// The expanded panel: big editable visualiser, every control of the effect,
// macro routing, mix, mode, wavetable / IR and slot management.
// ============================================================
class ExpandedPanel : public juce::Component
{
public:
    explicit ExpandedPanel (EfxGridProcessor& p);
    ~ExpandedPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;   // click on the scrim closes
    bool keyPressed (const juce::KeyPress&) override;

    void setSlot (int slot);
    int  shownSlot() const { return slot; }
    void refreshForEffect();                  // rebuild controls for the slot's effect
    void tick();                              // ~30 Hz: live values, dimming, labels

    std::function<void (int)> onNavSlot;      // -1 / +1
    std::function<void()> onClose;
    std::function<void()> onChanged;          // slot model changed (tiles must refresh)

private:
    void layoutPanel();
    void updateModeButtons();
    void loadTable();
    void loadIr();

    EfxGridProcessor& proc;
    int slot = 0, shownFx = -1;
    bool assignMode = false;

    juce::Rectangle<int> panelArea, headArea, vizArea, meterArea, controlsArea, rightCol, tableArea, scArea, routeArea, hintArea;

    VizEditor vizEditor;
    juce::TextButton pickBtn, bypassBtn { "ON" }, closeBtn { "x" }, prevBtn { "<" }, nextBtn { ">" };
    juce::Slider macroKnob, mixSlider;
    juce::Label macroCaption, mixCaption, modeCaption;
    juce::TextButton assignBtn { "ASSIGN" }, routeResetBtn { "DEFAULT" };
    juce::OwnedArray<juce::TextButton> modeBtns;
    juce::TextButton tableLoadBtn { "LOAD TABLE..." }, tableResetBtn { "BUILT-IN" };
    juce::TextButton irLoadBtn { "LOAD IR..." }, irClearBtn { "CLEAR" };
    juce::TextButton resetBtn { "RESET" }, moveLBtn { "< MOVE" }, moveRBtn { "MOVE >" }, removeBtn { "REMOVE" };
    juce::TextButton randBtn { "RANDOM" }, mutBtn { "MUTATE" };
    juce::OwnedArray<ParamControl> controls;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> macroAtt, mixAtt;
    std::unique_ptr<juce::ParameterAttachment> bypassAtt;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExpandedPanel)
};

// ============================================================
// The full editor: a slim header and the chain board. The board starts as a
// single "+"; each effect takes a cell, rows split equally between their
// cells, and a new row starts every four effects.
// ============================================================
class EfxGridEditor : public juce::AudioProcessorEditor,
                      public juce::DragAndDropContainer,
                      private juce::Timer
{
public:
    explicit EfxGridEditor (EfxGridProcessor&);
    // undo steps: a gesture is one step (committed on mouse up, scroll runs
    // merged); the editor timer commits menu / combo edits shortly after
    struct UndoSteps : juce::MouseListener
    {
        EfxGridProcessor& p;
        juce::uint32 lastTouch = 0;
        explicit UndoSteps (EfxGridProcessor& pr) : p (pr) {}
        void touch() { lastTouch = juce::Time::getMillisecondCounter(); }
        bool touchedRecently() const { return juce::Time::getMillisecondCounter() - lastTouch < 10000; }
        void mouseDown (const juce::MouseEvent&) override { touch(); }
        void mouseDrag (const juce::MouseEvent&) override { touch(); }
        void mouseUp (const juce::MouseEvent&) override { touch(); p.undoCheckpoint(); }
        void mouseDoubleClick (const juce::MouseEvent&) override { touch(); p.undoCheckpoint(); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override { touch(); p.undoCheckpoint (true); }
    };
    UndoSteps undoSteps;
    ~EfxGridEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    void openSlot (int slot);
    void closeOverlay();
    void refreshFromModel();     // resync every tile from the processor
    void addEffectMenu (juce::Component& target);

private:
    void timerCallback() override;
    void layoutBoard();

    EfxGridProcessor& proc;
    std::unique_ptr<PastelLookAndFeel> lnf;

    std::array<std::unique_ptr<GridTile>, EfxGridProcessor::kSlots> tiles;
    AddTile addTile;
    ExpandedPanel overlay;
    juce::TooltipWindow tooltips { this, 700 };

    juce::Rectangle<int> headerArea, boardArea;
    // header: preset bar, randomize / mutate for the whole chain
    juce::TextButton presetPrev { "<" }, presetNext { ">" }, presetBtn, randAllBtn { "RANDOM" }, mutAllBtn { "MUTATE" };
    pastel::MidiLearnButton midiBtn;
    PresetBrowser browser;
    juce::String shownPreset;
    void presetChanged();
public:
    PresetBrowser& presetBrowser() { return browser; }   // tests
    juce::TextButton& presetButton() { return presetBtn; }
private:
    int shownFx[EfxGridProcessor::kSlots] {};
    int shownGen = -1, shownLen = -1;
    bool overlayOpen = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EfxGridEditor)
};
