#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Presets.h"

class EfxGridProcessor;

// Preset browser overlay (after the Pastel drum synth's): search, FACTORY /
// USER tabs, the list (click loads, double-click closes), and an inline name
// field with SAVE (a second click confirms overwriting) and DELETE for user
// presets. Clicking the scrim closes it.
class PresetBrowser : public juce::Component,
                      private juce::ListBoxModel
{
public:
    explicit PresetBrowser (EfxGridProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void()> onClose;
    std::function<void()> onPresetChanged;   // after any load or save

    // for tests
    int numRows() const { return (int) rows.size(); }
    void selectRow (int r) { list.selectRow (r); }
    void showBank (int b) { activeBank = b; refreshBankButtons(); rebuildRows(); }
    void saveAs (const juce::String& name);

private:
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRow) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void rebuildRows();
    void refreshBankButtons();
    void clickSave();
    void setOverwriteArmed (bool on);

    EfxGridProcessor& proc;
    juce::TextEditor search, nameBox;
    juce::ListBox list { "presets", this };
    juce::TextButton closeBtn { "x" }, saveBtn { "SAVE" }, delBtn { "DELETE" }, folderBtn { "FOLDER" };
    juce::TextButton factoryTab { "FACTORY" }, userTab { "USER" };
    juce::ToggleButton keepSeq { "Keep sequence" };
    std::vector<presets::Entry> rows;
    int activeBank = 0;
    bool overwriteArmed = false, loadingRow = false;
    juce::Rectangle<int> panel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};
