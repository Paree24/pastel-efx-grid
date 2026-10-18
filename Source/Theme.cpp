#include "Theme.h"
#include "PastelAssets.h"

PastelLookAndFeel::PastelLookAndFeel()
{
    latoReg   = juce::Typeface::createSystemTypefaceFor(PastelAssets::LatoRegular_ttf,
                                                       PastelAssets::LatoRegular_ttfSize);
    latoBold  = juce::Typeface::createSystemTypefaceFor(PastelAssets::LatoBold_ttf,
                                                       PastelAssets::LatoBold_ttfSize);
    latoBlack = juce::Typeface::createSystemTypefaceFor(PastelAssets::LatoBlack_ttf,
                                                       PastelAssets::LatoBlack_ttfSize);
    setColour(juce::Label::textColourId, PastelColors::text);
    setColour(juce::ComboBox::textColourId, PastelColors::text);
    setColour(juce::ComboBox::backgroundColourId, PastelColors::comboBg);
    setColour(juce::ListBox::backgroundColourId, PastelColors::panel);
    setColour(juce::ListBox::textColourId, PastelColors::text);
    setColour(juce::TextEditor::backgroundColourId, PastelColors::comboBg);
    setColour(juce::TextEditor::textColourId, PastelColors::text);
    setColour(juce::TextEditor::outlineColourId, PastelColors::panelEdge);
    setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, PastelColors::track);
    setColour(juce::TextEditor::highlightColourId, PastelColors::accent.withAlpha(0.35f));
    setColour(juce::CaretComponent::caretColourId, PastelColors::accent);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TooltipWindow::backgroundColourId, PastelColors::comboBg);
    setColour(juce::TooltipWindow::textColourId, PastelColors::text);
    setColour(juce::TooltipWindow::outlineColourId, PastelColors::knobEdge);
    setColour(juce::BubbleComponent::backgroundColourId, PastelColors::comboBg);
    setColour(juce::BubbleComponent::outlineColourId, PastelColors::knobEdge);
    setColour(juce::Slider::textBoxTextColourId, PastelColors::text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, PastelColors::accent.withAlpha(0.35f));
    setColour(juce::ComboBox::outlineColourId, PastelColors::knobEdge);
    setColour(juce::PopupMenu::textColourId, PastelColors::text);
    setColour(juce::PopupMenu::headerTextColourId, PastelColors::textDim);
    setColour(juce::PopupMenu::backgroundColourId, PastelColors::panel);
    setColour(juce::AlertWindow::backgroundColourId, PastelColors::panel);
    setColour(juce::AlertWindow::textColourId, PastelColors::text);
    setColour(juce::AlertWindow::outlineColourId, PastelColors::panelEdge);
    setColour(juce::ResizableWindow::backgroundColourId, PastelColors::bg);
}

void PastelLookAndFeel::drawBubble(juce::Graphics& g, juce::BubbleComponent&, const juce::Point<float>&,
                                   const juce::Rectangle<float>& body)
{
    g.setColour(PastelColors::comboBg);
    g.fillRoundedRectangle(body, 4.0f);
    g.setColour(PastelColors::accent);
    g.drawRoundedRectangle(body.reduced(0.5f), 4.0f, 1.0f);
}

void PastelLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour(PastelColors::comboBg);
    g.fillRect(0, 0, w, h);
}

void PastelLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& te)
{
    g.setColour(te.hasKeyboardFocus(true) ? PastelColors::accent : PastelColors::panelEdge);
    g.drawRect(0, 0, w, h, 1);
}

juce::Font PastelLookAndFeel::uiFont(float h) const
{
    if (latoReg != nullptr) return juce::Font(juce::FontOptions(latoReg).withHeight(h));
    return juce::Font(juce::FontOptions(h));
}

juce::Font PastelLookAndFeel::titleFont(float h) const
{
    if (latoBold != nullptr) return juce::Font(juce::FontOptions(latoBold).withHeight(h));
    return juce::Font(juce::FontOptions(h, juce::Font::bold));
}

juce::Font PastelLookAndFeel::logoFont(float h) const
{
    if (latoBlack != nullptr) return juce::Font(juce::FontOptions(latoBlack).withHeight(h));
    return titleFont(h);
}

juce::Colour PastelLookAndFeel::accentFor(juce::Component& c)
{
    // nearest ancestor (or self) carrying an "accent" property wins
    for (juce::Component* p = &c; p != nullptr; p = p->getParentComponent())
    {
        auto v = p->getProperties()["accent"];
        if (v.isInt())
            return juce::Colour((juce::uint32)(int)v);
    }
    return PastelColors::accent;
}

juce::Font PastelLookAndFeel::getLabelFont(juce::Label& l) { return l.getFont(); } // honour setFont
juce::Font PastelLookAndFeel::getComboBoxFont(juce::ComboBox&) { return uiFont(13.0f); }

void PastelLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    // arrow is drawn in the last ~18 px; give the text everything else
    label.setBounds(1, 1, box.getWidth() - 20, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}
juce::Font PastelLookAndFeel::getPopupMenuFont() { return uiFont(13.5f); }
juce::Font PastelLookAndFeel::getTextButtonFont(juce::TextButton&, int h) { return titleFont(juce::jmin(12.0f, h * 0.48f)); }

void PastelLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                       float pos, float start, float end, juce::Slider& slider)
{
    const auto accent = PastelLookAndFeel::accentFor(slider);
    const auto& props = slider.getProperties();
    const bool assign = (bool) props["assign"];
    const float depth = props.contains("macroDepth") ? (float) props["macroDepth"] : 0.0f;
    const float eff = props.contains("effNorm") ? (float) props["effNorm"] : pos;

    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float r = juce::jmin(w, h) * 0.5f - 1.0f;
    float a = start + pos * (end - start);
    auto angle = [&](float t) { return start + juce::jlimit(0.0f, 1.0f, t) * (end - start); };

    float br = r - (r > 24.0f ? 8.0f : 5.0f);
    g.setColour(PastelColors::knobBody);
    g.fillEllipse(cx - br, cy - br, br * 2, br * 2);
    g.setColour(assign ? accent.withAlpha(0.7f) : PastelColors::knobEdge);
    g.drawEllipse(cx - br + 0.5f, cy - br + 0.5f, br * 2 - 1, br * 2 - 1, assign ? 1.5f : 1.0f);

    const float arcR = r - 1.5f, thick = r > 24.0f ? 3.0f : 2.5f;
    auto arc = [&](float a0, float a1, float rad, float th, juce::Colour c)
    {
        juce::Path p;
        p.addCentredArc(cx, cy, rad, rad, 0.0f, juce::jmin(a0, a1), juce::jmax(a0, a1), true);
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    };
    arc(start, end, arcR, thick, PastelColors::track);
    if (pos > 0.001f) arc(start, a, arcR, thick, accent);

    // macro routing: an outer arc from the knob value to where the macro takes it
    if (std::abs(depth) > 0.004f && r > 16.0f)
    {
        const float outer = arcR + (r > 24.0f ? 5.0f : 3.5f);
        arc(a, angle(pos + depth), outer, 2.0f, (depth > 0 ? juce::Colours::white : juce::Colour(0xFFE88A8A)).withAlpha(assign ? 0.95f : 0.55f));
    }
    // live (macro-modulated) value
    if (std::abs(eff - pos) > 0.003f)
    {
        const float ea = angle(eff);
        g.setColour(juce::Colours::white);
        g.fillEllipse(cx + std::sin(ea) * arcR - 2.5f, cy - std::cos(ea) * arcR - 2.5f, 5.0f, 5.0f);
    }

    float dx = std::cos(a - juce::MathConstants<float>::halfPi);
    float dy = std::sin(a - juce::MathConstants<float>::halfPi);
    g.setColour(accent);
    g.drawLine(cx + dx * 2.0f, cy + dy * 2.0f, cx + dx * (br - 2.0f), cy + dy * (br - 2.0f), 2.5f);
}

void PastelLookAndFeel::drawLinearSliderBackground(juce::Graphics&, int, int, int, int,
    float, float, float, const juce::Slider::SliderStyle, juce::Slider&) {}

void PastelLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                                       float pos, float, float,
                                       const juce::Slider::SliderStyle, juce::Slider& s)
{
    auto accent = PastelLookAndFeel::accentFor(s);
    if (s.isVertical())
    {
        // fader: wide track, outlined thumb with a grip line (readable on the dark panels)
        float cx = x + w * 0.5f;
        float top = y + 8.0f, bot = (float)(y + h - 8);
        g.setColour(PastelColors::track);
        g.fillRoundedRectangle(cx - 3.0f, top, 6.0f, bot - top, 3.0f);
        // JUCE passes the thumb position in pixels (not 0..1)
        float fy = juce::jlimit(top, bot, pos);
        g.setColour(accent);
        g.fillRoundedRectangle(cx - 3.0f, fy, 6.0f, bot - fy, 3.0f);
        juce::Rectangle<float> thumb(cx - 13.0f, fy - 7.0f, 26.0f, 14.0f);
        g.setColour(PastelColors::comboBg);
        g.fillRoundedRectangle(thumb, 3.0f);
        g.setColour(accent);
        g.drawRoundedRectangle(thumb.reduced(0.5f), 3.0f, 1.5f);
        g.fillRect(cx - 7.0f, fy - 1.0f, 14.0f, 2.0f);
    }
    else
    {
        float cy = y + h * 0.5f;
        float l = x + 4.0f, rr = (float)(x + w - 4);
        g.setColour(PastelColors::track);
        g.fillRoundedRectangle(l, cy - 1.5f, rr - l, 3.0f, 1.5f);
        float fx = juce::jlimit(l, rr, pos); // pixel position from JUCE
        g.setColour(accent);
        g.fillRoundedRectangle(l, cy - 1.5f, fx - l, 3.0f, 1.5f);
        g.setColour(PastelColors::comboBg);
        g.fillRoundedRectangle(fx - 6.0f, cy - 8.0f, 12.0f, 16.0f, 2.0f);
    }
}

void PastelLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    // textless pill switch: identical colors on/off, state shown by knob side
    bool on = b.getToggleState();
    float w = (float)b.getWidth(), h = (float)b.getHeight();
    float pw = juce::jmin(w - 4.0f, 40.0f), ph = juce::jmin(h - 4.0f, 16.0f);
    float px = (w - pw) * 0.5f, py = (h - ph) * 0.5f;
    g.setColour(PastelColors::track);
    g.fillRoundedRectangle(px, py, pw, ph, ph * 0.5f);
    g.setColour(PastelColors::knobEdge);
    g.drawRoundedRectangle(px + 0.5f, py + 0.5f, pw - 1, ph - 1, ph * 0.5f, 1.0f);
    float knobR = ph - 5.0f;
    float kx = on ? px + pw - knobR - 2.5f : px + 2.5f;
    g.setColour(PastelColors::text);
    g.fillEllipse(kx, py + 2.5f, knobR, knobR);
    juce::ignoreUnused(b);
}

void PastelLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool,
                                   int, int, int, int, juce::ComboBox& box)
{
    g.setColour(PastelColors::comboBg);
    g.fillRoundedRectangle(0, 0, w, h, 4.0f);
    g.setColour(box.hasKeyboardFocus(true) ? PastelLookAndFeel::accentFor(box) : PastelColors::knobEdge);
    g.drawRoundedRectangle(0.5f, 0.5f, w - 1, h - 1, 4.0f, 1.0f);
    juce::Path arrow;
    arrow.addTriangle(w - 16.0f, h * 0.5f - 3, w - 8.0f, h * 0.5f - 3, w - 12.0f, h * 0.5f + 3);
    g.setColour(PastelLookAndFeel::accentFor(box));
    g.fillPath(arrow);
    juce::ignoreUnused(box);
}

void PastelLookAndFeel::drawGroupComponentOutline(juce::Graphics& g, int w, int h,
                                         const juce::String& text,
                                         const juce::Justification&, juce::GroupComponent& group)
{
    g.setColour(PastelColors::panel);
    g.fillRoundedRectangle(0, 0, w, h, 5.0f);
    juce::Colour accent = PastelLookAndFeel::accentFor(group);
    g.setColour(accent);
    g.drawRoundedRectangle(0.5f, 0.5f, w - 1, h - 1, 5.0f, 1.0f);
    const float barH = 24.0f;
    g.setColour(accent);
    g.fillRoundedRectangle(1, 1, w - 2, barH, 4.0f);
    g.fillRect(1, (int)(barH - 5), w - 2, 5);
    g.setColour(PastelColors::barText);
    g.setFont(titleFont(13.0f));
    // keep clear of the title-bar toggle on the right; squeeze long titles
    g.drawFittedText(text.toUpperCase(), 10, 0, w - 66, (int)barH,
                     juce::Justification::centredLeft, 1, 0.75f);
}

void PastelLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b,
                                           const juce::Colour&, bool hover, bool down)
{
    const auto accent = PastelLookAndFeel::accentFor(b);
    auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const bool cl = b.isConnectedOnLeft(), cr = b.isConnectedOnRight();
    juce::Path shape;
    shape.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(), 5.0f, 5.0f,
                              ! cl, ! cr, ! cl, ! cr);
    g.setColour(down ? juce::Colour(0xFF2B2E34) : PastelColors::comboBg);
    g.fillPath(shape);
    if (b.getToggleState())
    {
        g.setColour(accent.withAlpha(0.85f));
        g.fillPath(shape);
        return;
    }
    g.setColour(hover ? accent.withAlpha(0.8f) : PastelColors::knobEdge);
    g.strokePath(shape, juce::PathStrokeType(1.0f));
}

void PastelLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.setColour(b.getToggleState() ? PastelColors::barText : (b.isEnabled() ? PastelColors::text : PastelColors::textDim));
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(4, 2), juce::Justification::centred, 1, 0.7f);
}

void PastelLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h)
{
    g.setColour(PastelColors::panel);
    g.fillRoundedRectangle(0, 0, w, h, 5.0f);
    g.setColour(PastelColors::panelEdge);
    g.drawRoundedRectangle(0.5f, 0.5f, w - 1, h - 1, 5.0f, 1.0f);
}

void PastelLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                        bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                                        bool hasSubMenu, const juce::String& text,
                                        const juce::String&, const juce::Drawable*, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour(PastelColors::track);
        g.fillRect(area.getX() + 8, area.getCentreY(), area.getWidth() - 16, 1);
        return;
    }
    const auto dot = textColour != nullptr ? *textColour : juce::Colour();
    if (isHighlighted && isActive)
    {
        g.setColour((textColour != nullptr ? dot : PastelColors::accent).withAlpha(0.2f));
        g.fillRoundedRectangle(area.toFloat().reduced(2.0f, 0.0f), 3.0f);
    }
    int x = area.getX() + 12;
    if (textColour != nullptr)
    {
        g.setColour(dot);
        g.fillEllipse((float) x, (float) area.getCentreY() - 3.5f, 7.0f, 7.0f);
        x += 14;
    }
    g.setColour(!isActive ? PastelColors::textDim.withAlpha(0.5f)
                : isHighlighted ? PastelColors::accentBright : PastelColors::text);
    g.setFont(isTicked ? titleFont(13.0f) : uiFont(13.0f));
    g.drawText(text, x, area.getY(), area.getRight() - x - 24, area.getHeight(), juce::Justification::centredLeft, true);
    if (isTicked)
    {
        g.setColour(textColour != nullptr ? dot : PastelColors::accent);
        g.fillEllipse((float) area.getRight() - 18.0f, (float) area.getCentreY() - 3.0f, 6.0f, 6.0f);
    }
    if (hasSubMenu)
    {
        juce::Path arrow;
        const float ax = (float) area.getRight() - 14.0f, ay = (float) area.getCentreY();
        arrow.addTriangle(ax, ay - 4.0f, ax, ay + 4.0f, ax + 5.0f, ay);
        g.setColour(PastelColors::textDim);
        g.fillPath(arrow);
    }
}

void PastelLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&,
                                      int x, int y, int w, int h, bool isVertical,
                                      int thumbStart, int thumbSize,
                                      bool, bool)
{
    g.setColour(PastelColors::track);
    if (isVertical)
        g.fillRoundedRectangle(x + w * 0.5f - 2, thumbStart, 4, thumbSize, 2.0f);
    else
        g.fillRoundedRectangle(thumbStart, y + h * 0.5f - 2, thumbSize, 4, 2.0f);
    juce::ignoreUnused(x); juce::ignoreUnused(y); juce::ignoreUnused(w); juce::ignoreUnused(h);
}
