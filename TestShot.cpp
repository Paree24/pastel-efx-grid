// Dev-only: renders the plugin editor offscreen to PNGs and sanity-checks the
// slot model (moves keep settings, state round-trips). Not shipped.
//
//   EditorShot <outdir>            grid + a set of expanded panels
//   EditorShot <outdir> all        an expanded panel for every effect
#include <algorithm>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "VizEditor.h"

static VizEditor* findVizEditor (juce::Component* c)
{
    if (auto* v = dynamic_cast<VizEditor*> (c)) return v;
    for (auto* ch : c->getChildren()) if (auto* v = findVizEditor (ch)) return v;
    return nullptr;
}

static juce::MouseEvent mouseAt (juce::Component* c, juce::Point<float> p, juce::Point<float> down, bool dragged)
{
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, c, c, juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), 1, dragged);
}

static void writePng (const juce::Image& img, const juce::File& out)
{
    juce::PNGImageFormat png;
    out.deleteFile();
    if (auto os = out.createOutputStream()) png.writeImageToStream (img, *os);
}

static int fails = 0;
static void check (bool ok, const juce::String& what)
{
    if (! ok) ++fails;
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
}

// CI: report a crash with a stack trace instead of dying silently
static void reportCrash (void*)
{
    std::printf ("\nCRASH\n%s\n", juce::SystemStats::getStackBacktrace().toRawUTF8());
    std::fflush (stdout);
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);   // unbuffered: CI keeps the output even if we crash
    juce::SystemStats::setApplicationCrashHandler (reportCrash);
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir = argc > 1 ? juce::File (argv[1]) : juce::File::getCurrentWorkingDirectory();
    const bool all = argc > 2 && juce::String (argv[2]) == "all";
    dir.createDirectory();

    EfxGridProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    long long pos = 0;
    const auto run = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 512; ++i, ++pos)
            {
                const double t = (double) pos / 48000.0;
                const float env = std::fmod (t * 2.0, 1.0) < 0.6 ? 1.0f : 0.15f;
                const float s = env * (float) (0.3 * std::sin (2 * juce::MathConstants<double>::pi * 220.0 * t)
                                             + 0.12 * std::sin (2 * juce::MathConstants<double>::pi * 1320.0 * t)
                                             + 0.05 * std::sin (2 * juce::MathConstants<double>::pi * 4400.0 * t));
                buf.setSample (0, i, s);
                buf.setSample (1, i, s * 0.8f + 0.05f * (float) std::sin (2 * juce::MathConstants<double>::pi * 330.0 * t));
            }
            proc.processBlock (buf, midi);
        }
    };

    // ---- slot model checks -------------------------------------------------
    std::printf ("== slot model ==\n");
    const int comp = efx::idForName ("Compressor"), eq = efx::idForName ("3-Band EQ"), dly = efx::idForName ("Simple Delay");
    proc.appendEffect (comp); proc.appendEffect (eq); proc.appendEffect (dly);
    proc.reloadSlotsNow();
    check (std::abs (proc.getNorm (0, 0) - efx::defForId (comp)->p[0].def) < 1.0e-4f, "loading applies the effect's defaults");
    proc.apvts.getParameter (EfxGridProcessor::paramId (1, "p0"))->setValueNotifyingHost (0.9f);
    proc.setMacroDepth (1, 2, -0.7f);
    proc.moveSlot (1, 0);
    check (proc.getSlotEffectId (0) == eq && proc.getSlotEffectId (1) == comp, "move reorders the chain");
    check (std::abs (proc.getNorm (0, 0) - 0.9f) < 1.0e-4f, "move carries knob values");
    check (std::abs (proc.getMacroDepth (0, 2) + 0.7f) < 1.0e-4f, "move carries macro routing");
    proc.clearSlotCompact (0);
    check (proc.getSlotEffectId (0) == comp && proc.getSlotEffectId (1) == dly && proc.chainLength() == 2, "remove packs the chain");

    // wavetable + state round trip
    const int wt = efx::idForName ("Wavetable Drive");
    proc.appendEffect (wt);
    {
        std::vector<float> saw (2048 * 4);
        for (size_t i = 0; i < saw.size(); ++i) saw[i] = (float) ((i % 2048) / 1024.0 - 1.0) * (float) (1 + i / 2048) / 4.0f;
        juce::File tmp = dir.getChildFile ("test_table.wav");
        tmp.deleteFile();
        juce::WavAudioFormat wav;
        if (auto os = std::unique_ptr<juce::OutputStream> (tmp.createOutputStream()))
            if (auto w = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (os.get(), 48000.0, 1, 16, {}, 0)))
            {
                os.release();
                const float* ch[1] = { saw.data() };
                w->writeFromFloatArrays (ch, 1, (int) saw.size());
            }
        juce::String err;
        check (proc.loadWavetable (2, tmp, err), "load a 4-frame wavetable " + err);
        check (proc.getWavetable (2)->numFrames == 4, "table has 4 frames");
        tmp.deleteFile();
    }
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    EfxGridProcessor proc2;
    proc2.prepareToPlay (48000.0, 512);
    proc2.setStateInformation (state.getData(), (int) state.getSize());
    check (proc2.getSlotEffectId (2) == wt && proc2.chainLength() == 3, "state restores the chain");
    check (std::abs (proc2.getMacroDepth (0, 2) - proc.getMacroDepth (0, 2)) < 1.0e-3f, "state restores macro routing");
    check (proc2.getWavetable (2)->numFrames == 4 && proc2.getWavetable (2)->name == "test_table", "state restores the embedded wavetable");

    // removing an effect leaves the audio untouched (Click Remover was reported
    // to keep acting after removal)
    for (int s = EfxGridProcessor::kSlots - 1; s >= 0; --s) if (proc.getSlotEffectId (s) > 0) proc.clearSlotCompact (s);
    for (const char* name : { "Click Remover", "Compressor", "Plate Reverb" })
    {
        proc.appendEffect (efx::idForName (name));
        proc.reloadSlotsNow();
        run (40);
        proc.clearSlotCompact (0);
        proc.reloadSlotsNow();
        run (80);    // tails / fades / lookahead flush
        juce::AudioBuffer<float> in (2, 512);
        for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (0.05 * (double) i); in.setSample (0, i, v); in.setSample (1, i, v); }
        juce::AudioBuffer<float> io (in);
        proc.processBlock (io, midi);
        float err = 0.0f;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) err = juce::jmax (err, std::abs (io.getSample (c, i) - in.getSample (c, i)));
        check (err < 1.0e-6f && proc.getLatencySamples() == 0, juce::String ("removing ") + name + " restores a clean pass-through (max err " + juce::String (err, 8) + ")");
    }

    // ---- screenshots ------------------------------------------------------
    for (int s = EfxGridProcessor::kSlots - 1; s >= 0; --s) if (proc.getSlotEffectId (s) > 0) proc.clearSlotCompact (s);
    {
        // the adaptive board: empty, 1, 2, 4, 5, 9 effects
        std::unique_ptr<juce::AudioProcessorEditor> e0 (proc.createEditorIfNeeded());
        auto* g0 = dynamic_cast<EfxGridEditor*> (e0.get());
        const char* seq[] = { "Compressor", "3-Band EQ", "Chorus", "Plate Reverb", "Saturation", "Trance Gate", "Simple Delay", "Multimode Filter", "Oscilloscope" };
        int added = 0;
        for (int want : { 0, 1, 2, 4, 5, 9 })
        {
            while (added < want) proc.appendEffect (efx::idForName (seq[added++]));
            proc.reloadSlotsNow();
            run (30);
            g0->refreshFromModel();
            g0->resized();
            writePng (g0->createComponentSnapshot (g0->getLocalBounds(), false), dir.getChildFile ("board_" + juce::String (want).paddedLeft ('0', 2) + ".png"));
        }
    }
    for (int s = EfxGridProcessor::kSlots - 1; s >= 0; --s) if (proc.getSlotEffectId (s) > 0) proc.clearSlotCompact (s);
    const char* showcase[] = { "Spectrum Analyzer", "Compressor", "3-Band EQ", "Multimode Filter", "Chorus", "Stereo Width",
                               "Pitch Correction", "Saturation", "Plate Reverb", "Ping-Pong Delay", "Spectral Comp", "Slicer",
                               "Transient Shaper", "Multiband Gate", "Harmonic Resonator", "Loudness Meter" };
    for (auto* n : showcase) proc.appendEffect (efx::idForName (n));
    proc.reloadSlotsNow();
    proc.apvts.getParameter (EfxGridProcessor::paramId (6, "bypass"))->setValueNotifyingHost (1.0f);
    run (300);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditorIfNeeded());
    auto* grid = dynamic_cast<EfxGridEditor*> (ed.get());
    grid->setSize (1180, 860);
    grid->refreshFromModel();
    writePng (grid->createComponentSnapshot (grid->getLocalBounds(), false), dir.getChildFile ("shot_grid.png"));

    grid->setSize (860, 640);
    writePng (grid->createComponentSnapshot (grid->getLocalBounds(), false), dir.getChildFile ("shot_grid_min.png"));
    grid->openSlot (1);
    run (20);
    writePng (grid->createComponentSnapshot (grid->getLocalBounds(), false), dir.getChildFile ("shot_panel_min.png"));
    grid->closeOverlay();
    grid->setSize (1180, 860);

    auto shootPanel = [&] (int slot, const juce::String& file)
    {
        grid->openSlot (slot);
        run (40);
        grid->refreshFromModel();
        writePng (grid->createComponentSnapshot (grid->getLocalBounds(), false), dir.getChildFile (file));
        grid->closeOverlay();
    };
    for (int s = 0; s < (int) std::size (showcase); ++s)
        shootPanel (s, "panel_" + juce::String (s).paddedLeft ('0', 2) + "_" + juce::String (showcase[s]).replaceCharacter (' ', '_').replaceCharacter ('/', '_') + ".png");

    // visual editing: drag across every editable visual and check the drags
    // reach the effect (parameters or shape data change, values stay legal)
    {
        std::printf ("== visual editing ==\n");
        int dead = 0;
        for (int id = 1; id <= efx::kNumEffects; ++id)
        {
            proc.loadEffect (0, id);
            proc.reloadSlotsNow();
            run (4);
            grid->openSlot (0);
            auto* ve = findVizEditor (grid);
            if (ve == nullptr) { check (false, "panel has a VizEditor"); break; }
            if (ve->hint().isEmpty()) { grid->closeOverlay(); continue; }
            auto snap = [&]
            {
                std::vector<float> v;
                for (int k = 0; k < efx::kMaxParams; ++k) v.push_back (proc.getNorm (0, k));
                for (int i = 0; i < efx::kDataSize; ++i) v.push_back (proc.getSlotData (0, i));
                return v;
            };
            const auto before = snap();
            const auto pr = viz::plotArea (ve->getLocalBounds().toFloat(), true);
            for (int gy = 0; gy < 5; ++gy)
                for (int gx = 0; gx < 7; ++gx)
                {
                    const juce::Point<float> p0 (pr.getX() + pr.getWidth() * (0.06f + 0.88f * (float) gx / 6.0f), pr.getY() + pr.getHeight() * (0.08f + 0.84f * (float) gy / 4.0f));
                    ve->mouseDown (mouseAt (ve, p0, p0, false));
                    for (int st = 1; st <= 4; ++st)
                        ve->mouseDrag (mouseAt (ve, p0 + juce::Point<float> (9.0f * (float) st, -7.0f * (float) st), p0, true));
                    ve->mouseUp (mouseAt (ve, p0 + juce::Point<float> (36.0f, -28.0f), p0, true));
                }
            // and every node / handle
            for (const auto& h : ve->handlePoints())
            {
                ve->mouseDown (mouseAt (ve, h, h, false));
                for (int st = 1; st <= 4; ++st)
                    ve->mouseDrag (mouseAt (ve, h + juce::Point<float> (-11.0f * (float) st, 8.0f * (float) st), h, true));
                ve->mouseUp (mouseAt (ve, h + juce::Point<float> (-44.0f, 32.0f), h, true));
            }
            const auto after = snap();
            bool changed = false, legal = true;
            for (size_t i = 0; i < after.size(); ++i)
            {
                if (std::abs (after[i] - before[i]) > 1.0e-5f) changed = true;
                if (! std::isfinite (after[i]) || (i < (size_t) efx::kMaxParams && (after[i] < 0.0f || after[i] > 1.0f))) legal = false;
            }
            run (8);   // the DSP survives the edited shapes
            float pk = 0.0f;
            for (int c = 0; c < 2; ++c) pk = juce::jmax (pk, buf.getMagnitude (c, 0, 512));
            if (! changed) ++dead;
            check (changed && legal && std::isfinite (pk) && pk < 16.0f,
                   juce::String ("drag-editing ") + efx::defForId (id)->name + (changed ? "" : " (nothing changed)") + (legal ? "" : " (illegal values)"));
            grid->closeOverlay();
        }
        std::printf ("%d editable visual(s) did not respond\n", dead);
    }

    // checks for the polish pass: synced delay drag, wah range handles,
    // undo of knob edits, control widths at the minimum window size
    {
        std::printf ("== polish checks ==\n");
        auto idOf = [] (const char* n) { for (int i = 1; i <= efx::kNumEffects; ++i) if (juce::String (efx::defForId (i)->name) == n) return i; return 0; };
        auto setN = [&] (int k, float n) { if (auto* p = proc.apvts.getParameter (EfxGridProcessor::paramId (0, "p" + juce::String (k)))) p->setValueNotifyingHost (n); };
        auto dragFirst = [&] (VizEditor* ve, int hi, juce::Point<float> by)
        {
            const auto hs = ve->handlePoints();
            if (hi >= (int) hs.size()) return false;
            const auto h = hs[(size_t) hi];
            ve->mouseDown (mouseAt (ve, h, h, false));
            for (int st = 1; st <= 4; ++st) ve->mouseDrag (mouseAt (ve, h + by * (float) st / 4.0f, h, true));
            ve->mouseUp (mouseAt (ve, h + by, h, true));
            return true;
        };

        // synced delay: sideways drag changes Div, not Time
        proc.loadEffect (0, idOf ("Simple Delay")); proc.reloadSlotsNow(); run (4);
        setN (0, 1.0f); run (4);
        grid->openSlot (0);
        if (auto* ve = findVizEditor (grid))
        {
            const float div0 = proc.getNorm (0, 2), t0 = proc.getNorm (0, 1);
            dragFirst (ve, 0, { -120.0f, 0.0f });
            check (proc.getNorm (0, 2) != div0 && proc.getNorm (0, 1) == t0, "synced delay drag steps the division (" + juce::String (div0) + " -> " + juce::String (proc.getNorm (0, 2)) + ")");
        }
        grid->closeOverlay();

        // wah auto: two range handles move Low / High
        proc.loadEffect (0, idOf ("Wah Wah")); proc.reloadSlotsNow(); proc.setSlotMode (0, 0); run (4);
        grid->openSlot (0);
        if (auto* ve = findVizEditor (grid))
        {
            check (ve->handlePoints().size() == 2 && ve->hint().isNotEmpty(), "wah auto has 2 range handles + hint");
            const float lo0 = proc.getNorm (0, 3), hi0 = proc.getNorm (0, 4);
            dragFirst (ve, 0, { 60.0f, 0.0f });
            dragFirst (ve, 1, { -60.0f, 0.0f });
            check (proc.getNorm (0, 3) != lo0 && proc.getNorm (0, 4) != hi0, "wah range handles set Low / High");
        }
        grid->closeOverlay();

        // undo / redo through the editor's keys: knobs, shape data, add / move / remove, wavetable reset
        {
            auto key = [&] (bool redo) { grid->keyPressed (juce::KeyPress ('z', redo ? juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier : juce::ModifierKeys::commandModifier, 0)); };
            for (int s = proc.chainLength() - 1; s >= 0; --s) proc.clearSlotCompact (s);
            proc.clearUndo();

            // add an effect, then a knob edit, then a shape-data edit
            proc.appendEffect (idOf ("Simple Delay"));
            const float g0 = proc.getNorm (0, 3);
            setN (3, g0 > 0.5f ? 0.1f : 0.9f); proc.undoCheckpoint();
            const float g1 = proc.getNorm (0, 3);
            proc.appendEffect (idOf ("Chorus"));
            const float d0 = proc.getSlotData (1, 0);
            proc.setSlotData (1, 0, d0 + 0.5f); proc.undoCheckpoint();

            key (false);
            check (std::abs (proc.getSlotData (1, 0) - d0) < 1e-6f, "undo: shape-data edit");
            key (false);
            check (proc.chainLength() == 1, "undo: added effect removed");
            key (false);
            check (std::abs (proc.getNorm (0, 3) - g0) < 1e-4f, "undo: knob edit");
            key (false);
            check (proc.chainLength() == 0, "undo: first effect removed");
            key (true); key (true);
            check (proc.chainLength() == 1 && std::abs (proc.getNorm (0, 3) - g1) < 1e-4f, "redo: first effect + knob back");
            key (true);
            check (proc.chainLength() == 2, "redo: second effect back");
            key (true);
            check (std::abs (proc.getSlotData (1, 0) - (d0 + 0.5f)) < 1e-6f, "redo: shape-data edit back");

            // move and remove
            proc.moveSlot (1, 0);
            check (proc.getSlotEffectId (0) == idOf ("Chorus"), "move applied");
            key (false);
            check (proc.getSlotEffectId (0) == idOf ("Simple Delay") && proc.getSlotEffectId (1) == idOf ("Chorus"), "undo: move");
            proc.clearSlotCompact (0);
            key (false);
            check (proc.chainLength() == 2 && proc.getSlotEffectId (0) == idOf ("Simple Delay") && std::abs (proc.getNorm (0, 3) - g1) < 1e-4f,
                   "undo: remove brings the effect back with its settings");
            // a new edit clears the redo history
            key (false);
            setN (3, 0.5f); proc.undoCheckpoint();
            check (! proc.canRedo(), "new edit clears redo");
            for (int s = proc.chainLength() - 1; s >= 0; --s) proc.clearSlotCompact (s);
            proc.clearUndo();
        }

        // reverb / delay: handles land under the mouse (fixed time axis while dragging)
        for (const char* name : { "Plate Reverb", "Simple Delay" })
        {
            proc.loadEffect (0, idOf (name)); proc.reloadSlotsNow(); run (4);
            if (juce::String (name) == "Simple Delay") setN (0, 0.0f);   // free time
            grid->openSlot (0);
            if (auto* ve = findVizEditor (grid))
            {
                const int hi = juce::String (name) == "Plate Reverb" ? 1 : 0;   // decay / time
                const auto h0 = ve->handlePoints()[(size_t) hi];
                const auto target = h0 + juce::Point<float> (-60.0f, 0.0f);
                ve->mouseDown (mouseAt (ve, h0, h0, false));
                ve->mouseDrag (mouseAt (ve, target, h0, true));
                const auto during = ve->handlePoints()[(size_t) hi];
                ve->mouseUp (mouseAt (ve, target, h0, true));
                check (std::abs (during.x - target.x) < 3.0f, juce::String (name) + ": handle follows the mouse (" + juce::String (during.x, 1) + " vs " + juce::String (target.x, 1) + ")");
            }
            grid->closeOverlay();
        }
        // delay feedback: drag the next-repeat handle up -> more feedback
        {
            proc.loadEffect (0, idOf ("Simple Delay")); proc.reloadSlotsNow(); run (4);
            grid->openSlot (0);
            if (auto* ve = findVizEditor (grid))
            {
                const float fb0 = proc.getNorm (0, 3);
                dragFirst (ve, 1, { 0.0f, -80.0f });
                check (proc.getNorm (0, 3) > fb0, "delay: dragging the repeat up raises feedback");
            }
            grid->closeOverlay();
        }
        // EQ Q brackets: dragging the right bracket outwards widens the mid band (lower Q)
        {
            proc.loadEffect (0, idOf ("3-Band EQ")); proc.reloadSlotsNow(); run (4);
            grid->openSlot (0);
            if (auto* ve = findVizEditor (grid))
            {
                const auto hp = ve->handlePoints();
                const float q0 = proc.getNorm (0, 6);
                check (hp.size() == 5, "3-band EQ: 3 nodes + 2 Q brackets");
                if (hp.size() == 5) dragFirst (ve, 4, { 70.0f, 0.0f });
                check (proc.getNorm (0, 6) < q0, "3-band EQ: Q bracket drag widens the band (" + juce::String (q0) + " -> " + juce::String (proc.getNorm (0, 6)) + ")");
            }
            grid->closeOverlay();
        }
        // harmonic resonator keyboard: a click makes the set Custom (from the
        // playing scale) and toggles that key; Low handle sets Low
        {
            proc.loadEffect (0, idOf ("Harmonic Resonator")); proc.reloadSlotsNow(); run (4);
            grid->openSlot (0);
            if (auto* ve = findVizEditor (grid))
            {
                const auto keys = viz::harmResKeys (viz::plotArea (ve->getLocalBounds().toFloat(), true), true);
                const auto fs = viz::harmResKeyRect (keys, 6).getCentre();   // F# (not in C major)
                ve->mouseDown (mouseAt (ve, fs, fs, false)); ve->mouseUp (mouseAt (ve, fs, fs, false));
                check (proc.getSlotMode (0) == 2 && proc.getSlotData (0, 6) > 0.5f && proc.getSlotData (0, 0) > 0.5f && proc.getSlotData (0, 1) < 0.5f,
                       "harmonic resonator: key click -> Custom, F# added, C kept, C# still off");
                const float lo0 = proc.getNorm (0, 3);
                dragFirst (ve, 0, { 80.0f, 0.0f });
                check (proc.getNorm (0, 3) > lo0, "harmonic resonator: Low handle drag raises Low");
            }
            grid->closeOverlay();
        }
        // multiband gate: threshold handle drag, crossover drag, wheel = range, double-click = band off
        {
            proc.loadEffect (0, idOf ("Multiband Gate")); proc.reloadSlotsNow(); run (4);
            grid->openSlot (0);
            if (auto* ve = findVizEditor (grid))
            {
                const auto hp = ve->handlePoints();
                check (hp.size() == 9, "multiband gate: 5 threshold + 4 crossover handles");
                const float th0 = proc.getSlotData (0, 2 * 3), x2 = proc.getNorm (0, 1);
                dragFirst (ve, 2, { 0.0f, -60.0f });
                check (proc.getSlotData (0, 2 * 3) > th0, "multiband gate: dragging band 3's line up raises its threshold");
                dragFirst (ve, 6, { 40.0f, 0.0f });   // handle 6 = crossover X2
                check (proc.getNorm (0, 1) > x2, "multiband gate: dragging a crossover moves it");
                const float rg = proc.getSlotData (0, 1);
                const auto h0 = ve->handlePoints()[0];
                juce::MouseWheelDetails w {}; w.deltaY = -0.5f;
                ve->mouseWheelMove (mouseAt (ve, h0, h0, false), w);
                check (proc.getSlotData (0, 1) < rg, "multiband gate: scrolling a band changes its range");
                ve->mouseDoubleClick (mouseAt (ve, h0, h0, false));
                check (proc.getSlotData (0, 2) < 0.5f && ve->handlePoints().size() == 8, "multiband gate: double-click switches a band off");
                ve->mouseDoubleClick (mouseAt (ve, h0, h0, false));
                check (proc.getSlotData (0, 2) > 0.5f, "multiband gate: double-click again switches it back on");
            }
            grid->closeOverlay();
        }
        // transient shaper envelope: decay point sideways sets Decay, tail end sets Release
        {
            proc.loadEffect (0, idOf ("Transient Shaper")); proc.reloadSlotsNow(); run (4);
            grid->openSlot (0);
            if (auto* ve = findVizEditor (grid))
            {
                const float d0 = proc.getNorm (0, 6), r0 = proc.getNorm (0, 5), a0 = proc.getNorm (0, 0);
                dragFirst (ve, 1, { 60.0f, 0.0f });
                check (proc.getNorm (0, 6) > d0, "transient: dragging the decay point right lengthens Decay");
                dragFirst (ve, 3, { -60.0f, 0.0f });
                check (proc.getNorm (0, 5) < r0, "transient: dragging the tail end left shortens Release");
                dragFirst (ve, 0, { 0.0f, -40.0f });
                check (proc.getNorm (0, 0) > a0, "transient: dragging the attack bump up raises Attack");
            }
            grid->closeOverlay();
        }
        // grid tile: macro knob drag, expand button opens the editor
        {
            proc.loadEffect (0, idOf ("Chorus")); proc.reloadSlotsNow(); run (4);
            grid->closeOverlay(); grid->resized();
            GridTile* tile = nullptr;
            for (auto* c : grid->getChildren()) if (auto* t = dynamic_cast<GridTile*> (c)) if (t->slot() == 0 && t->isVisible()) tile = t;
            if (tile == nullptr) check (false, "tile 0 found");
            else
            {
                const float m0 = proc.apvts.getRawParameterValue (EfxGridProcessor::paramId (0, "macro"))->load();
                const auto kc = tile->macroArea().getCentre();
                tile->mouseDown (mouseAt (tile, kc, kc, false));
                tile->mouseDrag (mouseAt (tile, kc + juce::Point<float> (0.0f, -60.0f), kc, true));
                tile->mouseUp (mouseAt (tile, kc + juce::Point<float> (0.0f, -60.0f), kc, true));
                const float m1 = proc.apvts.getRawParameterValue (EfxGridProcessor::paramId (0, "macro"))->load();
                check (m1 > m0 + 0.2f, "tile knob: drag up raises the macro (" + juce::String (m0) + " -> " + juce::String (m1) + ")");
                auto panelOpen = [&] { auto* v = findVizEditor (grid); return v != nullptr && v->getParentComponent()->isVisible(); };
                const bool openBefore = panelOpen();
                const auto ec = tile->expandArea().getCentre();
                tile->mouseDown (mouseAt (tile, ec, ec, false));
                tile->mouseUp (mouseAt (tile, ec, ec, false));
                check (! openBefore && panelOpen(), "tile expand button opens the editor");
                grid->closeOverlay();
            }
        }

        // presets: factory chains, user save / load round trip, stepping, undo
        {
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("pastel_efx_presets_test");
            tmp.deleteRecursively(); tmp.createDirectory();
            presets::setUserDirForTest (tmp);
            for (int s = proc.chainLength() - 1; s >= 0; --s) proc.clearSlotCompact (s);
            proc.appendEffect (idOf ("Chorus"));
            proc.clearUndo();
            int bad = 0;
            for (int i = 0; i < presets::numFactory(); ++i)
            {
                presets::applyFactory (proc, i);
                proc.reloadSlotsNow(); run (2);
                if (proc.presetName != presets::factoryName (i)) ++bad;
                for (int s = 0; s < proc.chainLength(); ++s) if (proc.getEffect (s) == nullptr) ++bad;
            }
            check (bad == 0, "presets: all " + juce::String (presets::numFactory()) + " factory chains load");
            auto factoryIdx = [] (const char* n) { for (int i = 0; i < presets::numFactory(); ++i) if (presets::factoryName (i) == n) return i; return -1; };
            presets::applyFactory (proc, factoryIdx ("Bass: Color"));
            check (proc.getSlotEffectId (0) == idOf ("Harmonic Resonator") && proc.getSlotEffectId (1) == idOf ("Saturation") && proc.getSlotEffectId (2) == idOf ("Mono Bass"),
                   "presets: Bass: Color = Harmonic Resonator > Saturation > Mono Bass");
            const float reso = proc.getNorm (0, 5);
            proc.undoStep();
            check (proc.getSlotEffectId (0) != idOf ("Harmonic Resonator") || proc.chainLength() != 3, "presets: loading is one undo step");
            proc.redoStep();
            // user round trip
            proc.setSlotData (0, 3, 0.5f); proc.undoCheckpoint();
            check (presets::saveUser ("My Bass", proc) && tmp.getChildFile ("My Bass.xml").existsAsFile(), "presets: user save writes the file");
            presets::applyFactory (proc, 0);   // Init
            check (proc.chainLength() == 0, "presets: Init clears the chain");
            const auto users = presets::scanUser();
            check (users.size() == 1 && presets::loadUser (users[0].file, proc), "presets: user preset listed and loads");
            check (proc.getSlotEffectId (0) == idOf ("Harmonic Resonator") && std::abs (proc.getNorm (0, 5) - reso) < 1e-5f
                   && std::abs (proc.getSlotData (0, 3) - 0.5f) < 1e-6f && proc.presetName == "My Bass",
                   "presets: user preset restores effects, knobs, shape data and name");
            // stepping wraps through factory + user
            presets::applyFactory (proc, presets::numFactory() - 1);
            presets::step (proc, 1);
            check (proc.presetName == "My Bass", "presets: next after the last factory is the user preset");
            presets::step (proc, 1);
            check (proc.presetName == presets::factoryName (0), "presets: next wraps to the first");
            presets::step (proc, -1);
            check (proc.presetName == "My Bass", "presets: previous wraps back");
            // browser: list + click loads
            auto& br = grid->presetBrowser();
            br.setVisible (true);
            br.showBank (0);
            check (br.numRows() == presets::numFactory(), "presets: browser lists the factory bank");
            br.selectRow (3);
            check (proc.presetName == presets::factoryName (3), "presets: clicking a row loads it");
            br.showBank (0);
            grid->resized();
            writePng (grid->createComponentSnapshot (grid->getLocalBounds(), false), dir.getChildFile ("shot_presets.png"));
            br.showBank (1);
            check (br.numRows() == 1, "presets: browser lists the user bank");
            br.setVisible (false);
            check (presets::deleteUser (tmp.getChildFile ("My Bass.xml")) && presets::scanUser().empty(), "presets: delete removes the user preset");
            tmp.deleteRecursively();
        }
        // old sessions: a v3 ADSR (effect #19) loads as a Transient Shaper with its decay / release
        {
            for (int s = proc.chainLength() - 1; s >= 0; --s) proc.clearSlotCompact (s);
            proc.appendEffect (idOf ("Harmonic Resonator"));   // id 19 = the old ADSR
            proc.appendEffect (idOf ("Transient Shaper"));
            const auto* ts = efx::defForId (idOf ("Transient Shaper"));
            auto xml = proc.presetXml();
            xml->setAttribute ("version", 3);
            for (auto* e : xml->getChildIterator())
            {
                const auto id = e->getStringAttribute ("id");
                // ADSR knobs (norm): Decay 1..2000 ms log -> 100 ms, Release 1..4000 ms log -> 400 ms
                if (id == EfxGridProcessor::paramId (0, "p1")) e->setAttribute ("value", std::log (100.0) / std::log (2000.0));
                if (id == EfxGridProcessor::paramId (0, "p3")) e->setAttribute ("value", std::log (400.0) / std::log (4000.0));
                if (id == EfxGridProcessor::paramId (1, "p6")) e->setAttribute ("value", 0.0);   // unused before v4
            }
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*xml, mb);
            proc.setStateInformation (mb.getData(), (int) mb.getSize());
            const float dec = efx::unmapParam (ts->p[6], proc.getNorm (0, 6)), rel = efx::unmapParam (ts->p[5], proc.getNorm (0, 5));
            check (proc.getSlotEffectId (0) == idOf ("Transient Shaper") && std::abs (dec - 100.0f) < 2.0f && std::abs (rel - 400.0f) < 5.0f,
                   "migration: v3 ADSR -> Transient Shaper (decay " + juce::String (dec, 1) + " ms, release " + juce::String (rel, 1) + " ms)");
            check (std::abs (proc.getNorm (1, 6) - ts->p[6].def) < 1e-5f, "migration: v3 Transient Shaper gets the default Decay");
            for (int s = proc.chainLength() - 1; s >= 0; --s) proc.clearSlotCompact (s);
            proc.clearUndo();
        }
        // factory presets on audio: sensible level, no clipping, dynamics kept
        {
            std::printf ("== factory presets on audio ==\n");
            auto drums = [] (double t)
            {
                const double beat = std::fmod (t * 2.0, 1.0), bar = std::fmod (t * 0.5, 1.0) * 4.0;
                const double kick = 0.8 * std::exp (-beat * 8.0) * std::sin (2 * juce::MathConstants<double>::pi * (50.0 * beat + 100.0 / 8.0 * (1.0 - std::exp (-beat * 8.0))));
                static juce::Random r (5);
                const double nz = r.nextFloat() * 2.0 - 1.0;
                const bool snareBeat = ((int) bar % 2) == 1;
                const double snare = snareBeat ? (0.45 * nz * std::exp (-beat * 20.0) + 0.3 * std::exp (-beat * 30.0) * std::sin (2 * juce::MathConstants<double>::pi * 200.0 * t)) : 0.0;
                const double eighth = std::fmod (t * 4.0, 1.0);
                static double prev = 0.0;
                const double hp = nz - prev; prev = nz;
                const double hat = 0.12 * hp * std::exp (-eighth * 60.0);
                return (float) (kick + snare + hat);
            };
            auto vocal = [] (double t)
            {
                const double syl = std::fmod (t * 3.3, 1.0);
                const double env = syl < 0.8 ? std::sin (juce::MathConstants<double>::pi * syl / 0.8) : 0.0;
                const double f0 = 220.0 * std::pow (2.0, (std::floor (t * 3.3) * 5.0 - 12.0 * std::floor (std::floor (t * 3.3) * 5.0 / 12.0)) / 12.0)
                                  * (1.0 + 0.01 * std::sin (2 * juce::MathConstants<double>::pi * 5.5 * t));
                double v = 0.0;
                for (int h = 1; h <= 24; ++h)
                {
                    const double fh = f0 * h;
                    const double formant = std::exp (-std::pow ((fh - 700.0) / 400.0, 2.0)) + 0.6 * std::exp (-std::pow ((fh - 1200.0) / 300.0, 2.0)) + 0.25 * std::exp (-std::pow ((fh - 2600.0) / 500.0, 2.0)) + 0.02;
                    v += formant / h * std::sin (2 * juce::MathConstants<double>::pi * fh * t);
                }
                static juce::Random r (9);
                const double sib = syl < 0.06 ? 0.15 * (r.nextFloat() * 2.0 - 1.0) : 0.0;
                return (float) (0.35 * env * v + sib);
            };
            auto mix = [&] (double t)
            {
                double chord = 0.0;
                for (double f : { 261.63, 329.63, 392.0, 493.88 })
                    for (int h = 1; h <= 6; ++h) chord += 0.04 / h * std::sin (2 * juce::MathConstants<double>::pi * f * h * t);
                const double bass = 0.25 * std::sin (2 * juce::MathConstants<double>::pi * 55.0 * t) * (0.6 + 0.4 * std::exp (-std::fmod (t * 2.0, 1.0) * 4.0));
                return 0.6f * drums (t) + (float) (chord + bass);
            };
            auto stats = [] (const std::vector<float>& x, double& rmsDb, double& peakDb, double& spreadDb)
            {
                double e = 0.0; float pk = 0.0f;
                std::vector<double> win;
                const size_t w = 480;
                for (size_t i = 0; i + w <= x.size(); i += w)
                {
                    double we = 0.0;
                    for (size_t j = i; j < i + w; ++j) { we += (double) x[j] * x[j]; pk = juce::jmax (pk, std::abs (x[j])); }
                    e += we;
                    win.push_back (10.0 * std::log10 (we / (double) w + 1e-12));
                }
                rmsDb = 10.0 * std::log10 (e / (double) std::max<size_t> (1, x.size()) + 1e-12);
                peakDb = 20.0 * std::log10 (pk + 1e-12);
                std::sort (win.begin(), win.end());
                // loud moments vs typical ones (95th vs 50th percentile of 10 ms levels)
                spreadDb = win.empty() ? 0.0 : win[(size_t) (win.size() * 95 / 100)] - win[win.size() / 2];
            };
            int badPresets = 0;
            for (int i = 1; i < presets::numFactory(); ++i)
            {
                presets::applyFactory (proc, i);
                proc.reloadSlotsNow();
                const auto tag = presets::factoryTag (i);
                const bool drumSig = tag == "Drums" || presets::factoryName (i) == "Drum Bus";
                const bool vocalSig = tag == "Vocal";
                std::vector<float> in, out;
                long long n0 = 0;
                for (int b = 0; b < 400; ++b)
                {
                    for (int k = 0; k < 512; ++k, ++n0)
                    {
                        const double t = (double) n0 / 48000.0;
                        const float v = 0.5f * (drumSig ? drums (t) : (vocalSig ? vocal (t) : mix (t)));   // peaks around -6 dBFS
                        buf.setSample (0, k, v); buf.setSample (1, k, v);
                        if (b >= 50) in.push_back (v);
                    }
                    proc.processBlock (buf, midi);
                    if (b >= 50) for (int k = 0; k < 512; ++k) out.push_back (buf.getSample (0, k));
                }
                bool finite = true;
                for (float v : out) if (! std::isfinite (v)) finite = false;
                double ri, pi, si, ro, po, so;
                stats (in, ri, pi, si); stats (out, ro, po, so);
                const double dLevel = ro - ri, squash = si - so;
                const bool analyze = tag == "Analyze", master = tag == "Master";
                const double maxSquash = master ? 4.0 : 6.0;
                const bool ok = finite && po <= 0.5 && std::abs (dLevel) <= (analyze ? 0.01 : 6.0) && squash <= maxSquash && ro > ri - 20.0;
                if (! ok) ++badPresets;
                std::printf ("  %s %-24s level %+5.1f dB   peak %+5.1f dBFS   dynamics %+5.1f dB\n",
                             ok ? "ok  " : "FAIL", presets::factoryName (i).toRawUTF8(), dLevel, po, -squash);
            }
            check (badPresets == 0, "factory presets: levels within 6 dB, no clipping, dynamics kept");
            for (int s2 = proc.chainLength() - 1; s2 >= 0; --s2) proc.clearSlotCompact (s2);
            proc.clearUndo();
        }
        // randomize / mutate
        {
            // Vocal: Pop Lead = 3-Band EQ, De-Esser, Compressor, Plate, Simple Delay (+ a meter)
            for (int i = 0; i < presets::numFactory(); ++i) if (presets::factoryName (i) == "Vocal: Pop Lead") presets::applyFactory (proc, i);
            proc.appendEffect (idOf ("Spectrum Analyzer"));
            proc.clearUndo();
            auto knobs = [&] { std::vector<float> v; for (int s = 0; s < proc.chainLength(); ++s) for (int k = 0; k < 16; ++k) v.push_back (proc.getNorm (s, k)); return v; };
            const auto before = knobs();
            const float compOut = proc.getNorm (2, 5), eqOut = proc.getNorm (0, 7);   // Makeup, Out
            proc.randomizeChain (0.6f);
            const auto after = knobs();
            int moved = 0, meterMoved = 0;
            for (size_t i = 0; i < before.size(); ++i) if (std::abs (before[i] - after[i]) > 1e-5f) { ++moved; if (i / 16 == 5) ++meterMoved; }
            check (moved >= 8 && meterMoved == 0, "randomize: knobs move (" + juce::String (moved) + "), the meter doesn't");
            check (std::abs (proc.getNorm (2, 5) - compOut) < 1e-6f && std::abs (proc.getNorm (0, 7) - eqOut) < 1e-6f, "randomize: output levels stay put");
            proc.undoStep();
            const auto undone = knobs();
            float worst = 0.0f;
            for (size_t i = 0; i < before.size(); ++i) worst = juce::jmax (worst, std::abs (before[i] - undone[i]));
            check (worst < 1e-5f, "randomize: one undo step restores every knob");
            proc.mutateChain (0.3f);
            const auto mutated = knobs();
            float maxStep = 0.0f; int changed = 0;
            for (size_t i = 0; i < before.size(); ++i)
            {
                const float dlt = std::abs (before[i] - mutated[i]);
                if (dlt > 1e-5f) ++changed;
                // continuous knobs only: a choice may flip to any option
                const auto* def = efx::defForId (proc.getSlotEffectId ((int) (i / 16)));
                const int k = (int) (i % 16);
                if (def != nullptr && k < def->numParams && ! def->p[k].isChoice()) maxStep = juce::jmax (maxStep, dlt);
            }
            check (changed >= 8 && maxStep <= 0.3f * 0.25f + 1e-4f, "mutate: small moves (max " + juce::String (maxStep, 3) + ")");
            proc.randomizeSlot (0, 0.8f);
            check (proc.presetName == "Randomized", "randomize: preset name shows it was randomized");
            proc.randomNewChain();
            bool noMeters = true;
            for (int s = 0; s < proc.chainLength(); ++s) if (efx::defForId (proc.getSlotEffectId (s))->cat == efx::CatAnalysis) noMeters = false;
            check (proc.chainLength() >= 3 && proc.chainLength() <= 5 && noMeters, "random new chain: 3-5 effects, no meters");
            proc.reloadSlotsNow(); run (8);
            for (int s = proc.chainLength() - 1; s >= 0; --s) proc.clearSlotCompact (s);
            proc.clearUndo();
        }

        // minimum window: every effect's controls get >= 78 px and stay inside the panel
        grid->setSize (860, 640); run (4);
        int narrow = 0, worstId = 0, worstW = 9999;
        for (int id = 1; id <= efx::kNumEffects; ++id)
        {
            proc.loadEffect (0, id); proc.reloadSlotsNow(); run (2);
            grid->openSlot (0);
            auto* ve = findVizEditor (grid);
            auto* panel = ve != nullptr ? ve->getParentComponent() : nullptr;
            if (panel != nullptr)
                for (auto* c : panel->getChildren())
                    if (dynamic_cast<ParamControl*> (c) != nullptr && c->isVisible())
                    {
                        if (c->getWidth() < worstW) { worstW = c->getWidth(); worstId = id; }
                        if (c->getWidth() < 78 || ! panel->getLocalBounds().contains (c->getBounds()) || c->getBounds().intersects (ve->getBounds())) ++narrow;
                    }
            grid->closeOverlay();
        }
        check (narrow == 0, juce::String (narrow) + " cramped/overlapping control(s) at 860x640, narrowest " + juce::String (worstW) + " px (" + efx::defForId (juce::jmax (1, worstId))->name + ")");
        grid->setSize (1180, 860); run (4);
    }

    if (all)
    {
        for (int id = 1; id <= efx::kNumEffects; ++id)
        {
            proc.loadEffect (0, id);
            proc.reloadSlotsNow();
            run (60);
            shootPanel (0, "all_" + juce::String (id).paddedLeft ('0', 2) + ".png");
        }
    }
    std::printf ("%d slot-model failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
