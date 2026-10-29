#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// ============================================================
// The Sequencer tab: when each slot's effect plays.
//   scene bar   on switch, 16 scene pads (playing lit, queued dashed),
//               Steps / Rate / Swing for the scene, dice and mutate
//   ruler       bar.beat over the steps, with the playhead
//   lanes       one row per slot in chain order: activity light, name,
//               Always / Seq, Cut / Ring, dice, then the step cells
// Gestures: click a cell to toggle it, drag to paint (the first cell
// decides on or off), Alt-drag to rotate a lane, drag a lane header onto
// another lane to copy it, right-click a header for clear / fill /
// euclidean / own steps / own rate, right-click a scene pad for copy /
// paste / clear.
// ============================================================
class SeqView : public juce::Component, private juce::Timer
{
public:
    explicit SeqView (EfxGridProcessor&);
    ~SeqView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void refresh();                         // the chain or the state changed
    int editScene() const;                  // the scene the lanes show (queued, else playing)

    // geometry, for tests (view coordinates)
    std::vector<int> laneSlots() const { return lanes; }
    juce::Rectangle<int> cellBounds (int row, int step) const;
    juce::Rectangle<int> headerBounds (int row) const;
    juce::Rectangle<int> modeButton (int row, int which) const;   // 0 Always, 1 Seq
    juce::Rectangle<int> tailButton (int row, int which) const;   // 0 Cut, 1 Ring
    juce::Rectangle<int> diceButton (int row) const;
    juce::Rectangle<int> scenePad (int scene) const;
    juce::Rectangle<int> powerButton() const { return power; }
    juce::Rectangle<int> lanesBounds() const { return laneArea; }

private:
    void timerCallback() override;
    void layoutRows();
    int rowAt (juce::Point<int>) const;
    int stepAt (int row, int x) const;
    int steps() const;
    void laneMenu (int row);
    void sceneMenu (int scene);

    EfxGridProcessor& proc;
    juce::ComboBox stepsBox, rateBox;
    juce::Slider swing;
    juce::TextButton diceBtn { "DICE" }, mutBtn { "MUTATE" };
    std::vector<int> lanes;                 // slot per row
    juce::Rectangle<int> sceneBar, power, ruler, laneArea;
    int rowH = 64, scroll = 0, cellsX = 0, cellsW = 0;
    // gesture
    enum class Drag { None, Paint, Rotate, Copy };
    Drag drag = Drag::None;
    int dragRow = -1, dragStartStep = 0, lastStep = -1;
    bool paintOn = true;
    uint32_t dragStartMask = 0;
    int clipboardScene = -1;
    int shownGen = -1, shownScene = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqView)
};
