#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

//==============================================================================
AudioProcessorValueTreeState::ParameterLayout VXProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto msRange = NormalisableRange<float> (100.0f, 5000.0f, 1.0f);
    msRange.setSkewForCentre (750.0f);

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::speed, 1 }, "Speed", msRange, 750.0f,
                    AudioParameterFloatAttributes().withLabel ("ms")
                        .withStringFromValueFunction ([] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 2) + " s" : String (roundToInt (v)) + " ms"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::smoothing, 1 }, "Smoothing",
                    NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
                    AudioParameterFloatAttributes().withLabel ("%")
                        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::catchPeaks, 1 }, "Catch Peaks",
                    NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
                    AudioParameterFloatAttributes().withLabel ("%")
                        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::range, 1 }, "Range",
                    NormalisableRange<float> (3.0f, 24.0f, 0.1f), 18.0f,
                    AudioParameterFloatAttributes().withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int) { return String::charToString (0x00b1) + String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::floorMode, 1 }, "Noisefloor Mode",
                    StringArray { "Auto", "Custom" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::floorDb, 1 }, "Noisefloor",
                    NormalisableRange<float> (-80.0f, -20.0f, 0.1f), -40.0f,
                    AudioParameterFloatAttributes().withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::targetMode, 1 }, "Target",
                    StringArray { "LUFS-I Match", "LUFS-I Custom", "LUFS-S Match", "LUFS-S Custom" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::target, 1 }, "Target Level",
                    NormalisableRange<float> (-32.0f, -10.0f, 0.1f), -23.0f,
                    AudioParameterFloatAttributes().withLabel ("LUFS")
                        .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " LUFS"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::output, 1 }, "Output",
                    NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
                    AudioParameterFloatAttributes().withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::bypass, 1 }, "Bypass", false));
    return layout;
}

//==============================================================================
VXProcessor::VXProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VXLevelerState", createLayout())
{
    pSpeed      = apvts.getRawParameterValue (ParamID::speed);
    pSmooth     = apvts.getRawParameterValue (ParamID::smoothing);
    pCatch      = apvts.getRawParameterValue (ParamID::catchPeaks);
    pRange      = apvts.getRawParameterValue (ParamID::range);
    pFloorMode  = apvts.getRawParameterValue (ParamID::floorMode);
    pFloor      = apvts.getRawParameterValue (ParamID::floorDb);
    pTargetMode = apvts.getRawParameterValue (ParamID::targetMode);
    pTarget     = apvts.getRawParameterValue (ParamID::target);
    pOutput     = apvts.getRawParameterValue (ParamID::output);
    pBypass     = apvts.getRawParameterValue (ParamID::bypass);

    startTimerHz (20);
}

VXProcessor::~VXProcessor()
{
    stopTimer();
}

vx::Settings VXProcessor::getSettings() const
{
    vx::Settings s;
    s.speedMs    = pSpeed->load();
    s.smoothing  = pSmooth->load() * 0.01f;
    s.catchPeaks = pCatch->load() * 0.01f;
    s.rangeDb    = pRange->load();
    s.floorAuto  = pFloorMode->load() < 0.5f;
    s.floorDb    = pFloor->load();
    s.targetMode = (vx::TargetMode) jlimit (0, 3, (int) std::lround (pTargetMode->load()));
    s.targetLufs = pTarget->load();
    return s;
}

double VXProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    if (getTailLengthSecondsForARA (tail))
        return tail;
    return 0.0;
}

bool VXProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo())
        return false;
    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == out;
}

//==============================================================================
void VXProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());

    realtime.prepare (sampleRate, jmax (1, getMainBusNumOutputChannels()));
    preparedRealtime = true;
    setLatencySamples (isBoundToARA() ? 0 : realtime.getLatencySamples());
}

void VXProcessor::releaseResources()
{
    releaseResourcesForARA();
}

void VXProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playheadSeconds = pos->getTimeInSeconds().orFallback (0.0);
            playing = pos->getIsPlaying();
        }
    }

    const bool bypass = pBypass->load() > 0.5f;
    const float outGain = Decibels::decibelsToGain (pOutput->load());

    if (isBoundToARA())
    {
        if (auto* r = getRenderer())
        {
            r->bypassed   = bypass;
            r->outputGain = outGain;
        }
        if (! processBlockForARA (buffer, isRealtime(), getPlayHead()))
            buffer.clear();
        return;
    }

    // ---------------- modo tempo real (host sem ARA)
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    if (! preparedRealtime) return;
    realtime.setSettings (getSettings());
    realtime.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), bypass);
    if (! bypass)
        buffer.applyGain (outGain);
    realtimeGainDb = realtime.getCurrentGainDb();
}

//==============================================================================
static std::shared_ptr<const vx::GainCurve> applyEdits (const vx::GainCurve& base,
                                                        const std::vector<float>& edits,
                                                        const vx::Analysis& analysis)
{
    auto c = std::make_shared<vx::GainCurve> (base);
    bool any = false;
    for (size_t i = 0; i < c->gainDb.size(); ++i)
    {
        const float e = i < edits.size() ? edits[i] : 0.0f;
        if (std::abs (e) > 1.0e-6f) any = true;
        const float g = jlimit (-48.0f, 24.0f, c->autoDb[i] + e);
        c->gainDb[i]  = g;
        c->gainLin[i] = vx::dbToLin (g);
    }
    if (any)
        c->outputLufsI = vx::integratedLoudness (analysis.ms, c->gainDb.data());
    return c;
}

void VXProcessor::refreshCurves()
{
    auto* renderer = getRenderer();
    if (renderer == nullptr)
    {
        if (! cache.empty()) { cache.clear(); ++curvesVersion; }
        return;
    }

    const auto settings = getSettings();
    bool changed = false;
    std::set<const ARAAudioModification*> present;

    for (auto* region : renderer->getPlaybackRegions())
    {
        auto* mod = dynamic_cast<VXAudioModification*> (region->getAudioModification());
        if (mod == nullptr) continue;
        auto* src = dynamic_cast<VXAudioSource*> (mod->getAudioSource());
        if (src == nullptr) continue;
        if (! present.insert (mod).second) continue;

        auto analysis = src->getAnalysis();
        auto& e = cache[mod];

        if (analysis == nullptr)
        {
            if (e.full != nullptr) changed = true;
            e = CacheEntry();
            continue;
        }

        mod->ensureSize (analysis->numFrames());

        if (e.source != src || e.base == nullptr || e.analysisVersion != src->getAnalysisVersion() || e.settings != settings)
        {
            e.source = src;
            e.base = vx::computeGainCurve (*analysis, settings, nullptr);
            e.analysisVersion = src->getAnalysisVersion();
            e.settings = settings;
            e.editsVersion = -1;
        }

        if (e.full == nullptr || e.editsVersion != mod->editsVersion)
        {
            e.full = applyEdits (*e.base, mod->editsDb, *analysis);
            e.editsVersion = mod->editsVersion;
            changed = true;
        }
    }

    for (auto it = cache.begin(); it != cache.end();)
    {
        if (present.count (it->first) == 0) { it = cache.erase (it); changed = true; }
        else ++it;
    }

    if (changed)
    {
        auto map = std::make_shared<VXCurveMap>();
        for (auto& [mod, e] : cache)
            if (e.full != nullptr) (*map)[mod] = e.full;
        renderer->publishCurves (map);
        ++curvesVersion;
    }
}

std::shared_ptr<const vx::GainCurve> VXProcessor::getCurveFor (const ARAAudioModification* mod) const
{
    auto it = cache.find (mod);
    return it != cache.end() ? it->second.full : nullptr;
}

//==============================================================================
void VXProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void VXProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* VXProcessor::createEditor()
{
    return new VXEditor (*this);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VXProcessor();
}
