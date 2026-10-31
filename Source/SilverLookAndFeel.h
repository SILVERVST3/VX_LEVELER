#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/** Paleta SILVER: prata/cinza claro com textos em azul-marinho. */
namespace Silver
{
    const juce::Colour bgTop        { 0xffe6e9ed };
    const juce::Colour bgBottom     { 0xffc4cad2 };
    const juce::Colour panel        { 0xffeef0f3 };
    const juce::Colour panelDark    { 0xffdde1e6 };
    const juce::Colour border       { 0xffa6aeb9 };
    const juce::Colour navy         { 0xff12284a };   // texto principal
    const juce::Colour navyMid      { 0xff1f3f73 };
    const juce::Colour steel        { 0xff5d6b7e };   // texto secundario
    const juce::Colour accent       { 0xff2457a6 };   // curva de ganho
    const juce::Colour contrast     { 0xffc8691c };   // edicoes manuais / pincel
    const juce::Colour waveOrig     { 0xffbcc4cf };
    const juce::Colour waveProc     { 0xff8299b9 };
    const juce::Colour grid         { 0xffc9ced6 };
    const juce::Colour good         { 0xff2f7d4f };
}

class SilverLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SilverLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& bg,
                               bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    static void drawMetalPanel (juce::Graphics&, juce::Rectangle<float> r, float corner = 8.0f);
};
