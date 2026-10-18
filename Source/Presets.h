#pragma once
#include <juce_core/juce_core.h>

class EfxGridProcessor;

// ============================================================
// Presets: factory chains built in code, and user presets saved as the full
// plugin state (XML) in Documents/Pastel EFX Grid Presets. Loading either is
// one undo step. (Same model as the Pastel drum synth's browser.)
// ============================================================
namespace presets
{
    struct Entry
    {
        bool isUser = false;
        int index = -1;            // factory: index into the factory list
        juce::String name, tag;
        juce::File file;           // user preset file
    };

    int numFactory();
    juce::String factoryName (int i);
    juce::String factoryTag (int i);
    bool applyFactory (EfxGridProcessor&, int i);

    void setUserDirForTest (const juce::File& dir);   // test harness override
    juce::File userDir();
    std::vector<Entry> scanUser();
    juce::String sanitiseName (juce::String name);
    bool saveUser (const juce::String& name, EfxGridProcessor&);
    bool loadUser (const juce::File& file, EfxGridProcessor&);
    bool deleteUser (const juce::File& file);

    // factory presets then user presets, in browser order
    std::vector<Entry> allEntries();
    bool loadEntry (const Entry&, EfxGridProcessor&);
    // step through allEntries() from the current preset (wraps)
    bool step (EfxGridProcessor&, int delta);
}
