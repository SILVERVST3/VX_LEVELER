#include "SilverLookAndFeel.h"

using namespace juce;

SilverLookAndFeel::SilverLookAndFeel()
{
    setColour (ResizableWindow::backgroundColourId, Silver::bgTop);
    setColour (Label::textColourId, Silver::navy);
    setColour (Slider::textBoxTextColourId, Silver::navy);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, Silver::accent.withAlpha (0.3f));
    setColour (Slider::rotarySliderFillColourId, Silver::navyMid);
    setColour (Slider::rotarySliderOutlineColourId, Silver::grid);
    setColour (TextButton::buttonColourId, Silver::panel);
    setColour (TextButton::buttonOnColourId, Silver::navyMid);
    setColour (TextButton::textColourOffId, Silver::navy);
    setColour (TextButton::textColourOnId, Colours::white);
    setColour (ComboBox::backgroundColourId, Silver::panel);
    setColour (ComboBox::textColourId, Silver::navy);
    setColour (ComboBox::outlineColourId, Silver::border);
    setColour (ComboBox::arrowColourId, Silver::navyMid);
    setColour (PopupMenu::backgroundColourId, Silver::panel);
    setColour (PopupMenu::textColourId, Silver::navy);
    setColour (PopupMenu::highlightedBackgroundColourId, Silver::navyMid);
    setColour (PopupMenu::highlightedTextColourId, Colours::white);
    setColour (TooltipWindow::backgroundColourId, Silver::navy);
    setColour (TooltipWindow::textColourId, Colours::white);
    setColour (TooltipWindow::outlineColourId, Silver::navy);
    setColour (TextEditor::backgroundColourId, Colours::white);
    setColour (TextEditor::textColourId, Silver::navy);
    setColour (TextEditor::highlightColourId, Silver::accent.withAlpha (0.3f));
    setColour (CaretComponent::caretColourId, Silver::navy);
    setColour (AlertWindow::backgroundColourId, Silver::panel);
    setColour (AlertWindow::textColourId, Silver::navy);
    setColour (AlertWindow::outlineColourId, Silver::border);
    setColour (ProgressBar::backgroundColourId, Silver::panelDark);
    setColour (ProgressBar::foregroundColourId, Silver::navyMid);
}

void SilverLookAndFeel::drawMetalPanel (Graphics& g, Rectangle<float> r, float corner)
{
    ColourGradient grad (Colours::white.withAlpha (0.85f), r.getX(), r.getY(),
                         Silver::panelDark, r.getX(), r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, corner);
    g.setColour (Colours::white.withAlpha (0.9f));
    g.drawRoundedRectangle (r.reduced (1.0f), corner - 1.0f, 1.0f);
    g.setColour (Silver::border);
    g.drawRoundedRectangle (r, corner, 1.0f);
}

void SilverLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, Slider& s)
{
    auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float size = jmin (bounds.getWidth(), bounds.getHeight());
    auto r = bounds.withSizeKeepingCentre (size, size);
    const auto c = r.getCentre();
    const float radius = size * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool enabled = s.isEnabled();

    // trilha do arco
    Path track;
    track.addCentredArc (c.x, c.y, radius - 2.0f, radius - 2.0f, 0.0f, startAngle, endAngle, true);
    g.setColour (Silver::grid);
    g.strokePath (track, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));

    // arco de valor (bipolar se o range cruza zero)
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float zeroPos = bipolar ? (float) s.valueToProportionOfLength (0.0) : 0.0f;
    const float a0 = startAngle + zeroPos * (endAngle - startAngle);
    Path val;
    val.addCentredArc (c.x, c.y, radius - 2.0f, radius - 2.0f, 0.0f, jmin (a0, angle), jmax (a0, angle), true);
    g.setColour (enabled ? Silver::navyMid : Silver::steel.withAlpha (0.4f));
    g.strokePath (val, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));

    // corpo metalico
    auto knob = r.reduced (size * 0.17f);
    g.setColour (Colours::black.withAlpha (0.18f));
    g.fillEllipse (knob.translated (0.0f, 2.0f));
    ColourGradient metal (Colours::white, knob.getX(), knob.getY(),
                          Colour (0xffaeb6c1), knob.getRight(), knob.getBottom(), false);
    metal.addColour (0.5, Colour (0xffe1e5ea));
    g.setGradientFill (metal);
    g.fillEllipse (knob);
    g.setColour (Silver::border.darker (0.2f));
    g.drawEllipse (knob, 1.0f);
    auto inner = knob.reduced (knob.getWidth() * 0.12f);
    ColourGradient metal2 (Colour (0xffd5dae1), inner.getX(), inner.getY(),
                           Colours::white, inner.getRight(), inner.getBottom(), false);
    g.setGradientFill (metal2);
    g.fillEllipse (inner);

    // ponteiro
    const float pr = inner.getWidth() * 0.5f;
    Line<float> pointer (c.getPointOnCircumference (pr * 0.25f, angle), c.getPointOnCircumference (pr * 0.92f, angle));
    g.setColour (enabled ? Silver::navy : Silver::steel);
    g.drawLine (pointer, 2.6f);
}

void SilverLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    if (on)
    {
        ColourGradient grad (Silver::navyMid.brighter (0.2f), r.getX(), r.getY(), Silver::navy, r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, 5.0f);
    }
    else
    {
        ColourGradient grad (Colours::white, r.getX(), r.getY(),
                             down ? Silver::panelDark.darker (0.08f) : Silver::panelDark, r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, 5.0f);
    }
    g.setColour (highlighted ? Silver::navyMid : Silver::border);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

void SilverLookAndFeel::drawButtonText (Graphics& g, TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.getToggleState() ? b.findColour (TextButton::textColourOnId)
                                    : (b.isEnabled() ? b.findColour (TextButton::textColourOffId) : Silver::steel.withAlpha (0.6f)));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), Justification::centred, 1);
}

Font SilverLookAndFeel::getTextButtonFont (TextButton&, int h)
{
    return Font (FontOptions (jmin (14.0f, (float) h * 0.5f), Font::bold));
}

void SilverLookAndFeel::drawComboBox (Graphics& g, int width, int height, bool, int, int, int, int, ComboBox& box)
{
    auto r = Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    ColourGradient grad (Colours::white, 0.0f, 0.0f, Silver::panelDark, 0.0f, (float) height, false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (box.hasKeyboardFocus (true) ? Silver::navyMid : Silver::border);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    Path arrow;
    const float ax = (float) width - 16.0f, ay = (float) height * 0.5f;
    arrow.addTriangle (ax - 5.0f, ay - 2.5f, ax + 5.0f, ay - 2.5f, ax, ay + 3.5f);
    g.setColour (Silver::navyMid);
    g.fillPath (arrow);
}

Font SilverLookAndFeel::getComboBoxFont (ComboBox&)
{
    return Font (FontOptions (14.0f, Font::bold));
}

void SilverLookAndFeel::positionComboBoxText (ComboBox& box, Label& label)
{
    label.setBounds (8, 1, box.getWidth() - 28, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setColour (Label::textColourId, Silver::navy);
}

void SilverLookAndFeel::drawPopupMenuBackground (Graphics& g, int width, int height)
{
    g.fillAll (Silver::panel);
    g.setColour (Silver::border);
    g.drawRect (0, 0, width, height);
}

Font SilverLookAndFeel::getLabelFont (Label& l)
{
    return l.getFont();
}
