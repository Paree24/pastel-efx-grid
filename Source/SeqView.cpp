#include "SeqView.h"
#include "Theme.h"

namespace
{
    juce::Font font (float h, bool bold = false) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }
    const juce::Colour kCellOff (0xFF24262B), kCellDim (0xFF1E2024), kHeader (0xFF1A1C20);

    juce::Colour laneColour (const EfxGridProcessor& p, int slot)
    {
        const auto* d = efx::defForId (p.getSlotEffectId (slot));
        return d != nullptr ? efx::catAccent ((int) d->cat) : PastelColors::accent;
    }

    // a two-option pill (ALWAYS | SEQ, CUT | RING)
    void segmented (juce::Graphics& g, juce::Rectangle<int> r, const char* a, const char* b, int on, juce::Colour accent, bool bEnabled = true)
    {
        g.setColour (juce::Colour (0xFF15161A));
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
        g.setColour (PastelColors::panelEdge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);
        auto left = r.withWidth (r.getWidth() / 2), right = r.withLeft (left.getRight());
        auto seg = (on == 0 ? left : right).reduced (2);
        g.setColour (accent.withAlpha (0.85f));
        g.fillRoundedRectangle (seg.toFloat(), 4.0f);
        g.setFont (font (9.5f, true));
        g.setColour (on == 0 ? PastelColors::barText : PastelColors::textDim);
        g.drawText (a, left, juce::Justification::centred, false);
        g.setColour (on == 1 ? PastelColors::barText : (bEnabled ? PastelColors::textDim : PastelColors::textDim.withAlpha (0.35f)));
        g.drawText (b, right, juce::Justification::centred, false);
    }
}

SeqView::SeqView (EfxGridProcessor& p) : proc (p)
{
    for (int i = 1; i <= seq::kMaxSteps; ++i) stepsBox.addItem (juce::String (i), i);
    for (int i = 0; i < seq::kNumRates; ++i) rateBox.addItem (seq::rateName (i), i + 1);
    stepsBox.onChange = [this]
    {
        auto ss = proc.sceneSettings (editScene());
        ss.steps = stepsBox.getSelectedId();
        proc.setSceneSettings (editScene(), ss); proc.seqEdited(); repaint();
    };
    rateBox.onChange = [this]
    {
        auto ss = proc.sceneSettings (editScene());
        ss.rate = rateBox.getSelectedId() - 1;
        proc.setSceneSettings (editScene(), ss); proc.seqEdited(); repaint();
    };
    swing.setSliderStyle (juce::Slider::LinearHorizontal);
    swing.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    swing.setRange (0.0, 75.0, 1.0);
    swing.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)) + "%"; };
    swing.updateText();
    swing.onValueChange = [this]
    {
        auto ss = proc.sceneSettings (editScene());
        ss.swing = (float) swing.getValue() * 0.01f;
        proc.setSceneSettings (editScene(), ss); proc.seqEdited(); repaint();
    };
    diceBtn.setTooltip ("New steps for every sequenced lane of this scene");
    mutBtn.setTooltip ("Flip a few steps of this scene");
    diceBtn.onClick = [this] { proc.diceScene (editScene()); refresh(); };
    mutBtn.onClick = [this] { proc.mutateScene (editScene(), 0.5f); refresh(); };
    stepsBox.setTooltip ("Steps in this scene's pattern (lanes can have their own: right-click a lane)");
    rateBox.setTooltip ("Step length");
    swing.setTooltip ("Delays every second step");
    setupBtn.setTooltip ("MIDI notes for scenes and live pads, gate fade, when scenes switch");
    setupBtn.onClick = [this] { settingsMenu(); };
    for (auto* c : std::initializer_list<juce::Component*> { &stepsBox, &rateBox, &swing, &diceBtn, &mutBtn, &setupBtn }) addAndMakeVisible (c);
    for (auto* b : { &diceBtn, &mutBtn, &setupBtn })
    {
        b->setColour (juce::TextButton::textColourOffId, PastelColors::text);
        b->setColour (juce::TextButton::textColourOnId, PastelColors::barText);
    }
    refresh();
    startTimerHz (30);
}

SeqView::~SeqView() = default;

int SeqView::editScene() const
{
    const int q = proc.queuedScene();
    return q >= 0 ? q : proc.playingScene();
}

int SeqView::steps() const { return proc.sceneSettings (editScene()).steps; }

void SeqView::refresh()
{
    lanes.clear();
    for (int s = 0; s < proc.chainLength(); ++s)
        if (const auto* d = efx::defForId (proc.getSlotEffectId (s)); d != nullptr && d->cat != efx::CatAnalysis)
            lanes.push_back (s);   // meters and scopes have nothing to switch
    const auto ss = proc.sceneSettings (editScene());
    stepsBox.setSelectedId (ss.steps, juce::dontSendNotification);
    rateBox.setSelectedId (ss.rate + 1, juce::dontSendNotification);
    swing.setValue (ss.swing * 100.0, juce::dontSendNotification);
    layoutRows();
    repaint();
}

void SeqView::timerCallback()
{
    // the playhead and lights move; the scene can change from MIDI / automation
    const int sc = editScene();
    if (sc != shownScene || proc.stateGeneration() != shownGen)
    {
        shownScene = sc;
        shownGen = proc.stateGeneration();
        refresh();
    }
    else repaint();
}

void SeqView::resized()
{
    auto r = getLocalBounds();
    // narrow windows: the scene pads get their own row
    const bool twoRows = r.getWidth() < 1100;
    sceneBar = r.removeFromTop (twoRows ? 84 : 48);
    r.removeFromTop (8);
    {
        auto b = sceneBar.reduced (12, 10);
        auto first = twoRows ? b.removeFromTop (28) : b;
        if (twoRows) b.removeFromTop (8);
        power = first.removeFromLeft (28);
        first.removeFromLeft (10);
        first.removeFromLeft (44);   // "SCENE"
        first.removeFromLeft (16 * 26);   // pads (painted)
        auto right = twoRows ? b : first;
        mutBtn.setBounds (right.removeFromRight (78)); right.removeFromRight (6);
        diceBtn.setBounds (right.removeFromRight (66)); right.removeFromRight (6);
        setupBtn.setBounds (right.removeFromRight (96)); right.removeFromRight (16);
        const int w = right.getWidth();
        // Steps / Rate / Swing, labelled in paint
        auto s1 = right.removeFromLeft (juce::jmin (130, w / 3));
        stepsBox.setBounds (s1.removeFromRight (64));
        auto s2 = right.removeFromLeft (juce::jmin (150, w / 3));
        rateBox.setBounds (s2.removeFromRight (84));
        auto s3 = right;
        swing.setBounds (s3.withTrimmedLeft (52));
    }
    laneArea = r;
    layoutRows();
}

void SeqView::layoutRows()
{
    const int headerW = 252;
    ruler = laneArea.withHeight (26);
    cellsX = laneArea.getX() + headerW + 16;
    cellsW = juce::jmax (40, laneArea.getRight() - 12 - cellsX);
    const int avail = laneArea.getHeight() - ruler.getHeight() - 8;
    const int n = juce::jmax (1, (int) lanes.size());
    rowH = juce::jlimit (56, 84, avail / n);   // two header lines need 56 px; more lanes than fit scroll
    const int maxScroll = juce::jmax (0, n * rowH - avail);
    scroll = juce::jlimit (0, maxScroll, scroll);
}

juce::Rectangle<int> SeqView::headerBounds (int row) const
{
    const int top = ruler.getBottom() + 4 + row * rowH - scroll;
    return { laneArea.getX() + 8, top + 3, cellsX - 16 - laneArea.getX() - 8, rowH - 6 };
}

juce::Rectangle<int> SeqView::cellBounds (int row, int step) const
{
    const int n = juce::jmax (1, steps());
    const float w = (float) cellsW / (float) n;
    const auto h = headerBounds (row);
    const int x0 = cellsX + (int) std::round (w * (float) step), x1 = cellsX + (int) std::round (w * (float) (step + 1));
    return { x0 + 2, h.getY(), juce::jmax (2, x1 - x0 - 4), h.getHeight() };
}

// the header's two lines (name, then the switches) sit together, centred in the row
static int controlsY (juce::Rectangle<int> h) { return h.getY() + juce::jmax (0, (h.getHeight() - 50) / 2) + 26; }
static int nameY (juce::Rectangle<int> h) { return h.getY() + juce::jmax (0, (h.getHeight() - 50) / 2); }

juce::Rectangle<int> SeqView::modeButton (int row, int which) const
{
    const auto h = headerBounds (row);
    auto r = juce::Rectangle<int> (h.getX() + 6, controlsY (h), 112, 22);
    return which == 0 ? r.withWidth (56) : r.withLeft (r.getX() + 56);
}

juce::Rectangle<int> SeqView::tailButton (int row, int which) const
{
    const auto h = headerBounds (row);
    auto r = juce::Rectangle<int> (h.getX() + 124, controlsY (h), 86, 22);
    return which == 0 ? r.withWidth (43) : r.withLeft (r.getX() + 43);
}

juce::Rectangle<int> SeqView::diceButton (int row) const
{
    const auto h = headerBounds (row);
    return { h.getX() + 216, controlsY (h), 22, 22 };
}

juce::Rectangle<int> SeqView::scenePad (int scene) const
{
    return { power.getRight() + 10 + 44 + scene * 26, power.getCentreY() - 11, 22, 22 };
}

void SeqView::paint (juce::Graphics& g)
{
    g.fillAll (PastelColors::bg);
    const int scene = editScene();
    const auto ss = proc.sceneSettings (scene);
    const int n = ss.steps;
    const seq::LaneClock sceneClock = seq::LaneClock::make (seq::Lane {}, ss);

    // ---- scene bar
    g.setColour (PastelColors::panel);
    g.fillRoundedRectangle (sceneBar.toFloat(), 8.0f);
    g.setColour (PastelColors::panelEdge);
    g.drawRoundedRectangle (sceneBar.toFloat().reduced (0.5f), 8.0f, 1.0f);
    {
        const bool on = proc.seqEnabled();
        auto pb = power.toFloat().withSizeKeepingCentre (24.0f, 24.0f);
        g.setColour (on ? PastelColors::accent.withAlpha (0.2f) : juce::Colour (0xFF1A1C21));
        g.fillEllipse (pb);
        g.setColour (on ? PastelColors::accent : PastelColors::textDim);
        g.drawEllipse (pb.reduced (0.5f), 1.4f);
        juce::Path pw;
        pw.addCentredArc (pb.getCentreX(), pb.getCentreY() + 0.5f, 5.5f, 5.5f, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
        g.strokePath (pw, juce::PathStrokeType (1.5f));
        g.fillRect (pb.getCentreX() - 0.75f, pb.getCentreY() - 6.5f, 1.5f, 6.0f);
        g.setColour (PastelColors::textDim);
        g.setFont (font (10.0f, true));
        g.drawText ("SCENE", power.getRight() + 10, power.getY(), 44, power.getHeight(), juce::Justification::centredLeft, false);
        const int playing = proc.playingScene(), queued = proc.queuedScene();
        for (int i = 0; i < seq::kScenes; ++i)
        {
            const auto r = scenePad (i).toFloat();
            const bool used = proc.sceneHasSteps (i);
            if (i == playing)
            {
                g.setColour (PastelColors::accent);
                g.fillRoundedRectangle (r, 4.0f);
            }
            else
            {
                g.setColour (juce::Colour (0xFF1A1C21));
                g.fillRoundedRectangle (r, 4.0f);
                g.setColour (PastelColors::panelEdge);
                if (i == queued)
                {
                    juce::Path outline; outline.addRoundedRectangle (r.reduced (0.5f), 4.0f);
                    juce::Path dashed;
                    const float dash[] = { 3.0f, 2.0f };
                    juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dash, 2);
                    g.setColour (PastelColors::accent);
                    g.fillPath (dashed);
                }
                else g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
            }
            g.setColour (i == playing ? PastelColors::barText : (used ? PastelColors::text : PastelColors::textDim.withAlpha (0.55f)));
            g.setFont (font (10.0f, true));
            g.drawText (juce::String (i + 1), r, juce::Justification::centred, false);
        }
        g.setColour (PastelColors::textDim);
        g.setFont (font (10.0f, true));
        g.drawText ("STEPS", stepsBox.getX() - 52, stepsBox.getY(), 48, stepsBox.getHeight(), juce::Justification::centredRight, false);
        g.drawText ("RATE", rateBox.getX() - 52, rateBox.getY(), 48, rateBox.getHeight(), juce::Justification::centredRight, false);
        g.drawText ("SWING", swing.getX() - 52, swing.getY(), 48, swing.getHeight(), juce::Justification::centredRight, false);
    }

    // ---- lanes panel
    auto panel = laneArea.toFloat();
    g.setColour (PastelColors::panel);
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour (PastelColors::panelEdge);
    g.drawRoundedRectangle (panel.reduced (0.5f), 8.0f, 1.0f);

    // ruler: bar.beat at each beat that starts on a step
    g.setFont (font (10.0f, true));
    for (int i = 0; i < n; ++i)
    {
        const double beat = (double) i * sceneClock.stepBeats;
        if (std::abs (beat - std::round (beat)) > 1.0e-6 && sceneClock.stepBeats < 1.0) continue;
        const int b = (int) std::round (beat);
        const auto c = cellBounds (0, i);
        const bool barStart = b % 4 == 0;
        g.setColour (barStart ? PastelColors::text : PastelColors::textDim);
        g.drawText (barStart ? juce::String (b / 4 + 1) : juce::String (b / 4 + 1) + "." + juce::String (b % 4 + 1),
                    c.getX(), ruler.getY() + 6, 40, 14, juce::Justification::centredLeft, false);
    }

    const bool running = proc.seqRunning();
    const double ppq = proc.seqPpq();
    const long long absStep = sceneClock.stepAt (ppq);
    const int curStep = (int) (((absStep % n) + n) % n);

    {
        juce::Graphics::ScopedSaveState clipLanes (g);
        g.reduceClipRegion (laneArea.withTrimmedTop (ruler.getHeight()).reduced (1));
        for (int row = 0; row < (int) lanes.size(); ++row)
        {
            const int slot = lanes[(size_t) row];
            const auto* d = efx::defForId (proc.getSlotEffectId (slot));
            if (d == nullptr) continue;
            const auto ln = proc.getSlotLanes (slot);
            const auto& lane = ln.lanes[scene];
            const auto colour = laneColour (proc, slot);
            const auto h = headerBounds (row);
            const bool always = ln.mode == seq::Always;
            const bool hasTail = proc.slotHasTail (slot);
            // header
            const bool lit = proc.slotGateOpen (slot) && (always || running);
            g.setColour (lit ? colour : juce::Colour (0xFF3A3D44));
            const int ny = nameY (h);
            g.fillEllipse ((float) h.getX() + 6.0f, (float) ny + 6.0f, 8.0f, 8.0f);
            if (! lit) { g.setColour (PastelColors::textDim); g.drawEllipse ((float) h.getX() + 6.0f, (float) ny + 6.0f, 8.0f, 8.0f, 1.0f); }
            g.setColour (PastelColors::text);
            g.setFont (font (13.0f, true));
            g.drawFittedText (d->name, h.getX() + 20, ny, 150, 20, juce::Justification::centredLeft, 1, 0.8f);
            g.setColour (juce::Colour (0xFF55585E));
            g.setFont (font (10.0f, true));
            g.drawText (juce::String (slot + 1).paddedLeft ('0', 2), h.getRight() - 24, ny, 20, 20, juce::Justification::centredRight, false);
            const int len = lane.len > 0 ? lane.len : n;
            if (lane.len > 0 || lane.rate >= 0)
            {
                const juce::String tag = juce::String (len) + juce::String::fromUTF8 (" \xc2\xb7 ") + seq::rateName (lane.rate >= 0 ? lane.rate : ss.rate);
                auto tr = juce::Rectangle<float> ((float) h.getX() + 130.0f, (float) ny + 1.0f, 58.0f, 18.0f);
                g.setColour (colour);
                g.drawRoundedRectangle (tr.reduced (0.5f), 9.0f, 1.0f);
                g.setFont (font (9.5f, true));
                g.drawText (tag, tr, juce::Justification::centred, false);
            }
            {
                segmented (g, modeButton (row, 0).getUnion (modeButton (row, 1)), "ALWAYS", "SEQ", always ? 0 : 1, colour);
                if (hasTail || ln.tail == seq::Ring)
                    segmented (g, tailButton (row, 0).getUnion (tailButton (row, 1)), "CUT", "RING", ln.tail, colour, hasTail);
                else
                    segmented (g, tailButton (row, 0).getUnion (tailButton (row, 1)), "CUT", "RING", 0, juce::Colour (0xFF55585E), false);
                const auto db = diceButton (row).toFloat();
                g.setColour (juce::Colour (0xFF15161A));
                g.fillRoundedRectangle (db, 5.0f);
                g.setColour (PastelColors::panelEdge);
                g.drawRoundedRectangle (db.reduced (0.5f), 5.0f, 1.0f);
                g.setColour (PastelColors::text);
                for (auto pt : { juce::Point<float> (-4, -4), juce::Point<float> (4, 4), juce::Point<float> (0, 0), juce::Point<float> (4, -4), juce::Point<float> (-4, 4) })
                    g.fillEllipse (db.getCentreX() + pt.x - 1.4f, db.getCentreY() + pt.y - 1.4f, 2.8f, 2.8f);
            }

            // cells
            const seq::LaneClock clock = seq::LaneClock::make (lane, ss);
            for (int i = 0; i < n; ++i)
            {
                const auto c = cellBounds (row, i).toFloat();
                const bool inLane = i < len;
                const bool on = clock.bit (i);
                juce::Colour fill = kCellOff;
                if (always) fill = colour.withAlpha (0.18f).overlaidWith (juce::Colours::transparentBlack);
                else if (on) fill = inLane ? colour.withAlpha (0.9f) : colour.withAlpha (0.4f);
                else if (! inLane) fill = kCellDim;
                g.setColour (always ? colour.withAlpha (0.2f) : fill);
                g.fillRoundedRectangle (c, 3.0f);
                // Ring: thin tail bars through the off cells after a run
                if (! always && ! on && ln.tail == seq::Ring && hasTail)
                {
                    int back = 0;
                    while (back < 3 && ! clock.bit (i - back - 1)) ++back;
                    if (back < 3 && clock.bit (i - back - 1))
                    {
                        g.setColour (colour.withAlpha (0.45f - 0.1f * (float) back));
                        g.fillRoundedRectangle (c.withSizeKeepingCentre (c.getWidth(), c.getHeight() * 0.28f), 2.0f);
                    }
                }
                if (running && i == curStep)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.18f));
                    g.drawRoundedRectangle (c.expanded (1.0f), 3.5f, 1.2f);
                }
            }
            if (lane.len > 0 && lane.len < n)
            {
                const auto c = cellBounds (row, lane.len);
                g.setColour (colour);
                g.fillRect (c.getX() - 3, c.getY() - 2, 2, c.getHeight() + 4);
            }
            if (always)
            {
                g.setColour (colour);
                g.setFont (font (10.0f, true));
                g.drawText ("ALWAYS ON", cellBounds (row, 0).getX() + 8, h.getY(), 120, h.getHeight(), juce::Justification::centredLeft, false);
            }
        }
    }

    if (lanes.empty())
    {
        g.setColour (PastelColors::textDim);
        g.setFont (font (13.0f));
        g.drawText ("Add effects on the Grid tab: each one gets a lane here", laneArea, juce::Justification::centred, false);
    }

    // playhead
    if (running)
    {
        const auto c0 = cellBounds (0, curStep);
        const double s0 = sceneClock.stepStart (absStep), s1 = sceneClock.stepStart (absStep + 1);
        const float frac = (float) juce::jlimit (0.0, 1.0, (ppq - s0) / juce::jmax (1.0e-9, s1 - s0));
        const float x = (float) c0.getX() - 2.0f + frac * (float) (c0.getWidth() + 4);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.fillRect (x - 0.5f, (float) ruler.getBottom() - 4.0f, 1.2f, (float) (laneArea.getBottom() - ruler.getBottom() - 4));
        juce::Path tri;
        tri.addTriangle (x - 5.0f, (float) ruler.getY() + 8.0f, x + 5.0f, (float) ruler.getY() + 8.0f, x, (float) ruler.getY() + 15.0f);
        g.fillPath (tri);
    }
}

int SeqView::rowAt (juce::Point<int> p) const
{
    for (int r = 0; r < (int) lanes.size(); ++r)
        if (p.y >= headerBounds (r).getY() - 3 && p.y < headerBounds (r).getY() - 3 + rowH) return r;
    return -1;
}

int SeqView::stepAt (int row, int x) const
{
    for (int i = 0; i < steps(); ++i)
    {
        const auto c = cellBounds (row, i);
        if (x >= c.getX() - 2 && x < c.getRight() + 2) return i;
    }
    return -1;
}

void SeqView::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::None;
    const auto p = e.getPosition();
    if (power.contains (p)) { proc.setSeqEnabled (! proc.seqEnabled()); repaint(); return; }
    for (int i = 0; i < seq::kScenes; ++i)
        if (scenePad (i).contains (p))
        {
            if (e.mods.isPopupMenu()) sceneMenu (i);
            else { proc.requestScene (i); refresh(); }
            return;
        }
    const int row = rowAt (p);
    if (row < 0) return;
    const int slot = lanes[(size_t) row];
    const int scene = editScene();
    if (headerBounds (row).contains (p))
    {
        if (e.mods.isPopupMenu()) { laneMenu (row); return; }
        {
            if (modeButton (row, 0).contains (p)) { proc.setLaneMode (slot, seq::Always); repaint(); return; }
            if (modeButton (row, 1).contains (p)) { proc.setLaneMode (slot, seq::Sequenced); repaint(); return; }
            if (tailButton (row, 0).contains (p)) { proc.setLaneTail (slot, seq::Cut); repaint(); return; }
            if (tailButton (row, 1).contains (p)) { proc.setLaneTail (slot, seq::Ring); repaint(); return; }
            if (diceButton (row).contains (p)) { proc.diceLane (slot, scene); refresh(); return; }
        }
        drag = Drag::Copy; dragRow = row;   // drop on another lane copies this one
        return;
    }
    const int st = stepAt (row, p.x);
    if (st < 0 || e.mods.isPopupMenu()) return;
    dragRow = row; dragStartStep = st; lastStep = st;
    dragStartMask = proc.laneSteps (slot, scene);
    if (e.mods.isAltDown()) { drag = Drag::Rotate; return; }
    drag = Drag::Paint;
    paintOn = ((dragStartMask >> st) & 1u) == 0;
    if (proc.getSlotLanes (slot).mode == seq::Always) proc.setLaneMode (slot, seq::Sequenced);   // drawing makes it a sequenced lane
    proc.setLaneSteps (slot, scene, paintOn ? (dragStartMask | (1u << (uint32_t) st)) : (dragStartMask & ~(1u << (uint32_t) st)));
    repaint();
}

void SeqView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragRow < 0 || dragRow >= (int) lanes.size()) return;
    const int slot = lanes[(size_t) dragRow];
    const int scene = editScene();
    if (drag == Drag::Paint)
    {
        const int st = stepAt (dragRow, e.x);
        if (st < 0 || st == lastStep) return;
        // fill the cells between the last one and this one, too
        uint32_t m = proc.laneSteps (slot, scene);
        for (int i = juce::jmin (st, lastStep); i <= juce::jmax (st, lastStep); ++i)
            m = paintOn ? (m | (1u << (uint32_t) i)) : (m & ~(1u << (uint32_t) i));
        lastStep = st;
        proc.setLaneSteps (slot, scene, m);
        repaint();
    }
    else if (drag == Drag::Rotate)
    {
        const auto ln = proc.getSlotLanes (slot).lanes[scene];
        const int len = ln.len > 0 ? ln.len : steps();
        const float cellW = (float) cellsW / (float) juce::jmax (1, steps());
        const int shift = (int) std::round ((float) e.getDistanceFromDragStartX() / cellW);
        uint32_t m = 0;
        for (int i = 0; i < len; ++i)
            if ((dragStartMask >> i) & 1u) m |= 1u << (uint32_t) (((i + shift) % len + len) % len);
        proc.setLaneSteps (slot, scene, m);
        repaint();
    }
}

void SeqView::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::Copy)
    {
        const int to = rowAt (e.getPosition());
        if (to >= 0 && to != dragRow && dragRow >= 0)
        {
            const int scene = editScene();
            const int from = lanes[(size_t) dragRow], dst = lanes[(size_t) to];
            const auto src = proc.getSlotLanes (from).lanes[scene];
            proc.setLaneSteps (dst, scene, src.steps);
            proc.setLaneTiming (dst, scene, src.len, src.rate);
            if (proc.getSlotLanes (dst).mode == seq::Always) proc.setLaneMode (dst, seq::Sequenced);
        }
    }
    drag = Drag::None;
    dragRow = -1;
    repaint();
}

void SeqView::laneMenu (int row)
{
    const int slot = lanes[(size_t) row];
    const int scene = editScene();
    const auto lane = proc.getSlotLanes (slot).lanes[scene];
    const int len = lane.len > 0 ? lane.len : steps();
    juce::PopupMenu m, euclid, own, rate;
    m.addItem (1, "Clear");
    m.addItem (2, "Fill");
    for (int k = 1; k <= len; ++k) euclid.addItem (100 + k, juce::String (k) + " pulses");
    m.addSubMenu ("Euclidean", euclid);
    own.addItem (200, "Scene's (" + juce::String (steps()) + ")", true, lane.len == 0);
    for (int k = 1; k <= seq::kMaxSteps; ++k) own.addItem (200 + k, juce::String (k), true, lane.len == k);
    m.addSubMenu ("Lane steps", own);
    rate.addItem (300, "Scene's", true, lane.rate < 0);
    for (int k = 0; k < seq::kNumRates; ++k) rate.addItem (301 + k, seq::rateName (k), true, lane.rate == k);
    m.addSubMenu ("Lane rate", rate);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [this, slot, scene, lane, len] (int r)
    {
        if (r == 0) return;
        if (r == 1) proc.setLaneSteps (slot, scene, 0u);
        else if (r == 2) proc.setLaneSteps (slot, scene, len >= 32 ? 0xFFFFFFFFu : ((1u << (uint32_t) len) - 1u));
        else if (r > 100 && r <= 132) proc.setLaneSteps (slot, scene, seq::euclid (len, r - 100, 0));
        else if (r >= 200 && r <= 232) proc.setLaneTiming (slot, scene, r - 200, lane.rate);
        else if (r >= 300) proc.setLaneTiming (slot, scene, lane.len, r - 301);
        if (r <= 132 && proc.getSlotLanes (slot).mode == seq::Always) proc.setLaneMode (slot, seq::Sequenced);
        proc.undoCheckpoint();
        refresh();
    });
}

void SeqView::sceneMenu (int scene)
{
    juce::PopupMenu m;
    m.addItem (1, "Copy");
    m.addItem (2, "Paste", clipboardScene >= 0 && clipboardScene != scene);
    m.addItem (3, "Clear");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [this, scene] (int r)
    {
        if (r == 1) clipboardScene = scene;
        else if (r == 2) proc.copyScene (clipboardScene, scene);
        else if (r == 3) proc.clearScene (scene);
        refresh();
    });
}

void SeqView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // more lanes than fit: the lane area scrolls
    if (! laneArea.contains (e.getPosition())) return;
    scroll -= (int) std::round (w.deltaY * 120.0f);
    layoutRows();
    repaint();
}

void SeqView::settingsMenu()
{
    static const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    auto noteName = [] (int n) { return juce::String (noteNames[n % 12]) + juce::String (n / 12 - 2); };   // 36 = C1
    juce::PopupMenu m, scenes, pads, fade, sw;
    for (int base : { 0, 12, 24, 36, 48, 60, 72, 84, 96 })
    {
        scenes.addItem (1000 + base, noteName (base) + " - " + noteName (base + 15), true, proc.sceneNoteBase() == base);
        pads.addItem (2000 + base, noteName (base) + " - " + noteName (base + 15), true, proc.padNoteBase() == base);
    }
    m.addSubMenu ("Scene notes", scenes);
    m.addSubMenu ("Live pad notes (hold to open a lane)", pads);
    for (int ms : { 1, 2, 3, 5, 10 }) fade.addItem (3000 + ms, juce::String (ms) + " ms", true, std::abs (proc.gateFadeMs() - (float) ms) < 0.01f);
    m.addSubMenu ("Gate fade", fade);
    sw.addItem (4000, "At the end of the pattern", true, proc.sceneSwitchMode() == EfxGridProcessor::SwitchPattern);
    sw.addItem (4001, "On the next beat", true, proc.sceneSwitchMode() == EfxGridProcessor::SwitchBeat);
    sw.addItem (4002, "Immediately", true, proc.sceneSwitchMode() == EfxGridProcessor::SwitchNow);
    m.addSubMenu ("Switch scenes", sw);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&setupBtn), [this] (int r)
    {
        if (r >= 4000) proc.setSceneSwitchMode (r - 4000);
        else if (r >= 3000) proc.setGateFadeMs ((float) (r - 3000));
        else if (r >= 2000) proc.setPadNoteBase (r - 2000);
        else if (r >= 1000) proc.setSceneNoteBase (r - 1000);
    });
}
