#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Dark flat design language (shared with Pastel Soundfont Player): near-black charcoal, thin panel borders,
// solid accent section bars with dark titles, needle knobs, Lato.
struct PastelColors
{
    static inline const juce::Colour bg        { 0xFF141518 };
    static inline const juce::Colour panel     { 0xFF1C1F24 };
    static inline const juce::Colour panelEdge { 0xFF33363C };
    static inline const juce::Colour text      { 0xFFD9D5D0 };
    static inline const juce::Colour textDim   { 0xFF8F8B85 };
    static inline const juce::Colour track     { 0xFF33363C };
    static inline const juce::Colour comboBg   { 0xFF232529 };
    static inline const juce::Colour knobBody  { 0xFF26292F };
    static inline const juce::Colour knobEdge  { 0xFF3A3D43 };
    static inline const juce::Colour barText   { 0xFF1B2336 };

    // single pastel-periwinkle accent family (Pastel Sampler; the player uses teal)
    static inline const juce::Colour accent       { 0xFF9EB9F3 };
    static inline const juce::Colour accentBright { 0xFFCDDBFA };
    static inline const juce::Colour accentDeep   { 0xFF7393DB };
    static inline const juce::Colour accentDark   { 0xFF45609F };
    // one functional exception: the root key / uncertain detections
    static inline const juce::Colour amber        { 0xFFE8B86A };
    static inline const juce::Colour keyWhite     { 0xFFD8D3C6 };
    static inline const juce::Colour keyOut       { 0xFF8E8A82 };
};

class PastelLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PastelLookAndFeel();

    juce::Font uiFont(float h) const;
    juce::Font titleFont(float h) const;
    juce::Font logoFont(float h) const;
    static juce::Colour accentFor(juce::Component& c);

    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h,
                          float pos, float start, float end, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h,
                          float pos, float min, float max,
                          const juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&,
                          bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh,
                      juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    void drawGroupComponentOutline(juce::Graphics&, int w, int h, const juce::String&,
                            const juce::Justification&, juce::GroupComponent&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                           bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override;
    void drawLinearSliderBackground(juce::Graphics&, int x, int y, int w, int h,
                                    float pos, float min, float max,
                                    const juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getLabelFont(juce::Label&) override;
    // route the default sans-serif face to the embedded Lato (needs setDefaultLookAndFeel)
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& f) override
    {
        if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName() && latoReg != nullptr)
            return f.isBold() ? latoBold : latoReg;
        return juce::LookAndFeel_V4::getTypefaceForFont(f);
    }
    juce::Font getSliderPopupFont(juce::Slider&) override { return uiFont(12.5f); }
    void drawBubble(juce::Graphics&, juce::BubbleComponent&, const juce::Point<float>& tip,
                    const juce::Rectangle<float>& body) override;
    void fillTextEditorBackground(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h,
                       bool isVertical, int thumbStart, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override;

private:
    juce::Typeface::Ptr latoReg, latoBold, latoBlack;
};
