#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "SilverLookAndFeel.h"

//==============================================================================
class LabeledKnob : public juce::Component
{
public:
    LabeledKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& title);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::Slider slider;
private:
    juce::String title;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
/** Janela de edicao: forma de onda original/processada + curva de ganho editavel. */
class LevelerView : public juce::Component,
                    private juce::Timer
{
public:
    explicit LevelerView (VXProcessor& p);
    ~LevelerView() override;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void undo();
    void clearEdits();
    void fitView() { autoFit = true; repaint(); }
    bool canUndo() const { return ! undoStack.empty(); }

    struct Lane
    {
        juce::ARAPlaybackRegion* region = nullptr;
        VXAudioModification* mod = nullptr;
        VXAudioSource* source = nullptr;
        double playStart = 0, playEnd = 0, modStart = 0;
        juce::String name;
    };
    std::vector<Lane> getLanes() const;

    /** clipe sob o cursor de reproducao (ou o primeiro) - para os medidores */
    std::shared_ptr<const vx::GainCurve> getFocusCurve() const;

    std::function<void()> onEditsChanged;

    /** dados prontos para desenhar um clipe (tambem usado pela ferramenta de captura de tela) */
    struct DrawLane
    {
        std::shared_ptr<const vx::Analysis> analysis;
        std::shared_ptr<const vx::GainCurve> curve;
        double playStart = 0, playEnd = 0, modStart = 0;
        juce::String name;
        int state = 0;
        float progress = 0.0f;
    };
    std::vector<DrawLane> previewLanes;   // so para desenvolvimento

private:
    void timerCallback() override;
    void paintARA (juce::Graphics&, juce::Rectangle<int> area);
    void paintRealtime (juce::Graphics&, juce::Rectangle<int> area);
    void paintGrid (juce::Graphics&, juce::Rectangle<int> area, float scaleDb);
public:
    static juce::Colour regionColour (int type);
private:

    juce::Rectangle<int> getPlotArea() const;
    float xForTime (double t) const;
    double timeForX (float x) const;
    float yForGain (float db) const;
    float gainScaleDb() const;
    void ensureView (const std::vector<DrawLane>& lanes);

    VXProcessor& proc;
    double viewStart = 0.0, viewEnd = 10.0;
    bool autoFit = true;

    double brushSeconds = 0.8;
    bool hovering = false;
    float hoverX = 0.0f;

    // edicao
    struct Snapshot { VXAudioModification* mod; std::vector<float> edits; };
    std::vector<Snapshot> dragSnapshot;
    std::vector<std::vector<Snapshot>> undoStack;
    bool dragging = false;
    double dragCentre = 0.0;
    float dragStartY = 0.0f;

    int lastCurvesVersion = -1;
    int lastModelVersion = -1;
};

//==============================================================================
class VXEditor : public juce::AudioProcessorEditor,
                 public juce::AudioProcessorEditorARAExtension,
                 private juce::Timer
{
public:
    explicit VXEditor (VXProcessor&);
    ~VXEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum Section { gateSec = 0, dynSec, breathSec, sibSec, numSections };

    void timerCallback() override;
    void exportWav();
    void exportMidi();
    void updateEnablement();
    void showSection (int index);
    void addCombo (juce::ComboBox& box, const juce::StringArray& items, const char* paramID,
                   std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& att,
                   const juce::String& label, int section);

    VXProcessor& proc;
    SilverLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    LevelerView view;

    int currentSection = dynSec;
    juce::TextButton tabs[numSections];
    juce::TextButton power[numSections];
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAtt[numSections];

    juce::ComboBox targetBox, floorBox, gateModeBox, breathModeBox, sibModeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> targetAtt, floorAtt, gateModeAtt, breathModeAtt, sibModeAtt;

    LabeledKnob speed, smoothing, catchPeaks, range, targetLevel, floorKnob, output;
    LabeledKnob gateThresh, gateRed, gateMinLen, gateClose, gateOpen;
    LabeledKnob breathRed, breathThresh, breathTrans;
    LabeledKnob sibThresh, sibTrans, sibSpectral;

    struct ComboRow { juce::ComboBox* box; juce::String label; int section; };
    std::vector<ComboRow> comboRows;
    std::vector<std::pair<juce::Component*, int>> sectionComps;

    juce::TextButton bypassButton { "Bypass" }, undoButton { "Undo" }, clearButton { "Clear Edits" },
                     fitButton { "Fit" }, reanalyseButton { "Reanalisar" }, wavButton { "Export WAV" }, midiButton { "Export MIDI" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;

    std::unique_ptr<juce::FileChooser> chooser;

    juce::String statusText, modeText, diagText;
    float inLufs = vx::kMinDb, outLufs = vx::kMinDb, floorVal = -40.0f, targetVal = -23.0f, gainNow = 0.0f;
    int nGate = 0, nBreath = 0, nSib = 0;

    juce::Rectangle<int> headerArea, sideArea, statsArea, toolbarArea, titleArea, descArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VXEditor)
};
