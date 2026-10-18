#include "PresetBrowser.h"
#include "PluginProcessor.h"
#include "Theme.h"

namespace
{
    juce::Font font (float h, bool bold = false) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }

    void styleField (juce::TextEditor& t, const juce::String& hint)
    {
        t.setTextToShowWhenEmpty (hint, PastelColors::textDim.withAlpha (0.7f));
        t.setColour (juce::TextEditor::backgroundColourId, PastelColors::comboBg);
        t.setColour (juce::TextEditor::textColourId, PastelColors::text);
        t.setColour (juce::TextEditor::outlineColourId, PastelColors::panelEdge);
        t.setColour (juce::TextEditor::focusedOutlineColourId, PastelColors::accent);
        t.setFont (font (14.0f));
    }
}

PresetBrowser::PresetBrowser (EfxGridProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    styleField (search, "Search presets...");
    search.onTextChange = [this] { rebuildRows(); };
    addAndMakeVisible (search);

    factoryTab.onClick = [this] { showBank (0); };
    userTab.onClick = [this] { showBank (1); };
    for (auto* b : { &factoryTab, &userTab })
    {
        b->setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (*b);
    }
    refreshBankButtons();

    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setRowHeight (28);
    addAndMakeVisible (list);

    styleField (nameBox, "Preset name...");
    nameBox.onTextChange = [this] { setOverwriteArmed (false); };
    nameBox.onReturnKey = [this] { clickSave(); };
    addAndMakeVisible (nameBox);

    closeBtn.onClick = [this] { if (onClose) onClose(); };
    saveBtn.onClick = [this] { clickSave(); };
    delBtn.setTooltip ("Delete the selected user preset");
    delBtn.onClick = [this]
    {
        const int r = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (r, (int) rows.size()) || ! rows[(size_t) r].isUser) return;
        if (presets::deleteUser (rows[(size_t) r].file)) { proc.presetIndex = -1; rebuildRows(); }
    };
    folderBtn.setTooltip ("Show the user preset folder");
    folderBtn.onClick = []
    {
        const auto d = presets::userDir();
        if (! d.isDirectory()) d.createDirectory();
        d.startAsProcess();
    };
    for (auto* b : { &closeBtn, &saveBtn, &delBtn, &folderBtn })
    {
        b->setColour (juce::TextButton::buttonColourId, PastelColors::comboBg);
        b->setColour (juce::TextButton::textColourOffId, PastelColors::text);
        addAndMakeVisible (*b);
    }
    rebuildRows();
}

// SAVE (inline, no pop-up): saves under the name field. An existing user
// preset needs a second click ("OVERWRITE?"); editing the name disarms it.
void PresetBrowser::clickSave()
{
    if (nameBox.getText().trim().isEmpty()) { nameBox.grabKeyboardFocus(); return; }
    const auto name = presets::sanitiseName (nameBox.getText());
    const bool exists = presets::userDir().getChildFile (name + ".xml").existsAsFile();
    if (exists && ! overwriteArmed) { setOverwriteArmed (true); return; }
    setOverwriteArmed (false);
    saveAs (name);
}

void PresetBrowser::setOverwriteArmed (bool on)
{
    overwriteArmed = on;
    saveBtn.setButtonText (on ? "OVERWRITE?" : "SAVE");
    saveBtn.setColour (juce::TextButton::buttonColourId, on ? PastelColors::accent.withAlpha (0.35f) : PastelColors::comboBg);
}

void PresetBrowser::saveAs (const juce::String& name)
{
    if (! presets::saveUser (name, proc)) return;
    search.setText ({}, false);
    nameBox.setText (proc.presetName, false);
    showBank (1);
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].name == proc.presetName) { juce::ScopedValueSetter<bool> sv (loadingRow, true); list.selectRow (i, false, true); }
    if (onPresetChanged) onPresetChanged();
}

void PresetBrowser::visibilityChanged()
{
    if (! isVisible()) return;
    // prefill with the current name, not generated ones
    const auto n = proc.presetName;
    const bool generated = n == "Init" || n.startsWith ("Randomized") || n.startsWith ("Mutated") || n.startsWith ("Random");
    nameBox.setText (generated ? juce::String() : n, false);
    setOverwriteArmed (false);
    activeBank = proc.presetIndex >= presets::numFactory() ? 1 : 0;
    refreshBankButtons();
    rebuildRows();
    search.grabKeyboardFocus();
}

void PresetBrowser::refreshBankButtons()
{
    for (auto [b, on] : { std::pair<juce::TextButton*, bool> { &factoryTab, activeBank == 0 }, { &userTab, activeBank == 1 } })
    {
        b->setToggleState (on, juce::dontSendNotification);   // the theme fills "on" buttons with the accent
        b->setColour (juce::TextButton::textColourOnId, PastelColors::barText);
        b->setColour (juce::TextButton::textColourOffId, PastelColors::textDim);
    }
    delBtn.setEnabled (activeBank == 1);
}

void PresetBrowser::rebuildRows()
{
    rows.clear();
    const auto q = search.getText().trim().toLowerCase();
    auto matches = [&q] (const presets::Entry& e) { return q.isEmpty() || e.name.toLowerCase().contains (q) || e.tag.toLowerCase().contains (q); };
    if (activeBank == 1) { for (auto& u : presets::scanUser()) if (matches (u)) rows.push_back (u); }
    else for (int i = 0; i < presets::numFactory(); ++i)
    {
        presets::Entry e { false, i, presets::factoryName (i), presets::factoryTag (i), {} };
        if (matches (e)) rows.push_back (e);
    }
    list.updateContent();
    // show the current preset selected, without loading it again
    juce::ScopedValueSetter<bool> sv (loadingRow, true);
    list.deselectAllRows();
    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto& r = rows[(size_t) i];
        const int idx = r.isUser ? presets::numFactory() + r.index : r.index;
        if (idx == proc.presetIndex) list.selectRow (i, false, true);
    }
    list.repaint();
}

void PresetBrowser::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size())) return;
    const auto& r = rows[(size_t) row];
    if (selected)
    {
        g.setColour (PastelColors::accent.withAlpha (0.16f));
        g.fillRoundedRectangle (2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 4.0f);
    }
    g.setColour (selected ? PastelColors::accent : PastelColors::textDim.withAlpha (0.8f));
    g.setFont (font (12.0f));
    g.drawText (r.isUser ? "USR" : juce::String (r.index + 1).paddedLeft ('0', 2), 10, 0, 40, h, juce::Justification::centredLeft, false);
    g.setColour (selected ? PastelColors::text : PastelColors::text.withAlpha (0.85f));
    g.setFont (font (14.0f, selected));
    g.drawText (r.name, 54, 0, w - 200, h, juce::Justification::centredLeft, true);
    g.setColour (PastelColors::textDim.withAlpha (0.7f));
    g.setFont (font (12.0f));
    g.drawText (r.tag, w - 140, 0, 130, h, juce::Justification::centredRight, false);
}

void PresetBrowser::selectedRowsChanged (int)
{
    if (loadingRow) return;
    const int r = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (r, (int) rows.size())) return;
    if (presets::loadEntry (rows[(size_t) r], proc))
    {
        if (! (proc.presetName == "Init")) nameBox.setText (rows[(size_t) r].isUser ? proc.presetName : juce::String(), false);
        if (onPresetChanged) onPresetChanged();
    }
    list.repaint();
}

void PresetBrowser::listBoxItemDoubleClicked (int, const juce::MouseEvent&)
{
    if (onClose) onClose();
}

bool PresetBrowser::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
    return false;
}

void PresetBrowser::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));   // scrim
    const auto pf = panel.toFloat();
    g.setColour (PastelColors::panel);
    g.fillRoundedRectangle (pf, 10.0f);
    g.setColour (PastelColors::panelEdge);
    g.drawRoundedRectangle (pf.reduced (0.5f), 10.0f, 1.0f);
    g.setColour (PastelColors::text);
    g.setFont (font (15.0f, true));
    g.drawText ("PRESETS", panel.getX() + 16, panel.getY() + 10, 200, 24, juce::Justification::centredLeft, false);
    g.setColour (PastelColors::textDim);
    g.setFont (font (11.0f));
    g.drawText ("Click to load, double-click to load and close. Save stores the whole chain.", panel.getX() + 16, panel.getBottom() - 74, panel.getWidth() - 32, 16, juce::Justification::centredLeft, false);
}

void PresetBrowser::resized()
{
    panel = juce::Rectangle<int> (0, 0, juce::jmin (640, getWidth() - 40), juce::jmin (560, getHeight() - 40)).withCentre (getLocalBounds().getCentre());
    closeBtn.setBounds (panel.getRight() - 40, panel.getY() + 10, 26, 24);
    search.setBounds (panel.getX() + 16, panel.getY() + 44, panel.getWidth() - 32, 28);
    factoryTab.setBounds (panel.getX() + 12, panel.getY() + 78, 96, 24);
    userTab.setBounds (panel.getX() + 112, panel.getY() + 78, 80, 24);
    list.setBounds (panel.getX() + 12, panel.getY() + 108, panel.getWidth() - 24, panel.getHeight() - 108 - 82);
    auto bottom = juce::Rectangle<int> (panel.getX() + 16, panel.getBottom() - 46, panel.getWidth() - 32, 30);
    folderBtn.setBounds (bottom.removeFromRight (80)); bottom.removeFromRight (8);
    delBtn.setBounds (bottom.removeFromRight (80)); bottom.removeFromRight (8);
    saveBtn.setBounds (bottom.removeFromRight (110)); bottom.removeFromRight (8);
    nameBox.setBounds (bottom);
}

void PresetBrowser::mouseDown (const juce::MouseEvent& e)
{
    if (! panel.contains (e.getPosition()) && onClose) onClose();   // scrim click closes
}
