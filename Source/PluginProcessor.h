#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VXDocumentController.h"
#include "LevelerDSP.h"
#include <map>
#include <set>

namespace ParamID
{
    static constexpr const char* speed      = "speed";
    static constexpr const char* smoothing  = "smooth";
    static constexpr const char* catchPeaks = "catch";
    static constexpr const char* range      = "range";
    static constexpr const char* floorMode  = "floorMode";
    static constexpr const char* floorDb    = "floor";
    static constexpr const char* targetMode = "targetMode";
    static constexpr const char* target     = "target";
    static constexpr const char* output     = "output";
    static constexpr const char* bypass     = "bypass";
}

//==============================================================================
class VXProcessor : public juce::AudioProcessor,
                    public juce::AudioProcessorARAExtension,
                    private juce::Timer
{
public:
    VXProcessor();
    ~VXProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "VX_Leveler SILVER"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    vx::Settings getSettings() const;

    /** ARA: curva atual de um clipe (thread de mensagens) */
    std::shared_ptr<const vx::GainCurve> getCurveFor (const juce::ARAAudioModification* mod) const;

    /** recalcula as curvas agora (chamado pelo editor durante edicoes) */
    void refreshCurves();
    int getCurvesVersion() const noexcept { return curvesVersion; }

    VXPlaybackRenderer* getRenderer() const noexcept { return getPlaybackRenderer<VXPlaybackRenderer>(); }

    vx::RealtimeLeveler& getRealtime() noexcept { return realtime; }

    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<bool>   playing { false };
    std::atomic<float>  realtimeGainDb { 0.0f };

private:
    void timerCallback() override { refreshCurves(); }

    struct CacheEntry
    {
        const juce::ARAAudioSource* source = nullptr;
        int analysisVersion = -1;
        int editsVersion = -1;
        vx::Settings settings;
        std::shared_ptr<const vx::GainCurve> base, full;
    };
    std::map<const juce::ARAAudioModification*, CacheEntry> cache;
    int curvesVersion = 0;

    vx::RealtimeLeveler realtime;
    bool preparedRealtime = false;

    std::atomic<float>* pSpeed = nullptr;
    std::atomic<float>* pSmooth = nullptr;
    std::atomic<float>* pCatch = nullptr;
    std::atomic<float>* pRange = nullptr;
    std::atomic<float>* pFloorMode = nullptr;
    std::atomic<float>* pFloor = nullptr;
    std::atomic<float>* pTargetMode = nullptr;
    std::atomic<float>* pTarget = nullptr;
    std::atomic<float>* pOutput = nullptr;
    std::atomic<float>* pBypass = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VXProcessor)
};
