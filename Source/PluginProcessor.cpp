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

    auto dbStr  = [] (float v, int) { return v <= vx::kInfDb + 0.05f ? String ("-inf dB") : String (v, 1) + " dB"; };
    auto msStr  = [] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 2) + " s" : String (roundToInt (v)) + " ms"; };
    auto pctStr = [] (float v, int) { return String (roundToInt (v)) + " %"; };
    auto msRangeOf = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi, 1.0f);
        r.setSkewForCentre (centre);
        return r;
    };

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::dynOn, 1 }, "Dynamics On", true));

    // ---- GATE
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { ParamID::gateOn, 1 }, "Gate On", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::gateMode, 1 }, "Gate Mode",
                    StringArray { "Vocal Detection", "Threshold Based" }, 0));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::gateThresh, 1 }, "Gate Threshold",
                    NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -40.0f,
                    AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbStr)));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::gateClose, 1 }, "Gate Close Speed",
                    msRangeOf (10.0f, 750.0f, 150.0f), 150.0f, AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msStr)));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::gateRed, 1 }, "Gate Reduction",
                    NormalisableRange<float> (vx::kInfDb, 0.0f, 0.1f), vx::kInfDb,
                    AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbStr)));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::gateOpen, 1 }, "Gate Open Speed",
                    msRangeOf (10.0f, 750.0f, 150.0f), 150.0f, AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msStr)));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::gateMinLen, 1 }, "Gate Min Length",
                    msRangeOf (50.0f, 3000.0f, 400.0f), 250.0f, AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msStr)));

    // ---- BREATH
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { ParamID::breathOn, 1 }, "Breath On", true));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::breathRed, 1 }, "Breath Reduction",
                    NormalisableRange<float> (vx::kInfDb, 0.0f, 0.1f, 2.0f), -3.0f,
                    AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbStr)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::breathMode, 1 }, "Breath Threshold Mode",
                    StringArray { "Auto", "Custom" }, 0));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::breathThresh, 1 }, "Breath Threshold",
                    NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -40.0f,
                    AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbStr)));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::breathTrans, 1 }, "Breath Transition",
                    msRangeOf (20.0f, 400.0f, 80.0f), 25.0f, AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msStr)));

    // ---- SIBILANCE
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { ParamID::sibOn, 1 }, "Sibilance On", true));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamID::sibMode, 1 }, "Sibilance Threshold Mode",
                    StringArray { "Auto", "Custom" }, 0));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::sibThresh, 1 }, "Sibilance Threshold",
                    NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -24.0f,
                    AudioParameterFloatAttributes().withLabel ("dBFS").withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dBFS"; })));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::sibTrans, 1 }, "Sibilance Transition",
                    msRangeOf (1.0f, 400.0f, 40.0f), 20.0f, AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msStr)));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { ParamID::sibSpectral, 1 }, "Spectral Sensitivity",
                    NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
                    AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctStr)));
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

    for (auto* id : { ParamID::dynOn, ParamID::gateOn, ParamID::gateMode, ParamID::gateThresh, ParamID::gateClose,
                      ParamID::gateRed, ParamID::gateOpen, ParamID::gateMinLen, ParamID::breathOn, ParamID::breathRed,
                      ParamID::breathMode, ParamID::breathThresh, ParamID::breathTrans, ParamID::sibOn, ParamID::sibMode,
                      ParamID::sibThresh, ParamID::sibTrans, ParamID::sibSpectral })
        raw[id] = apvts.getRawParameterValue (id);

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

    s.dynOn          = pv (ParamID::dynOn) > 0.5f;
    s.gateOn         = pv (ParamID::gateOn) > 0.5f;
    s.gateMode       = pv (ParamID::gateMode) > 0.5f ? 1 : 0;
    s.gateThreshDb   = pv (ParamID::gateThresh);
    s.gateCloseMs    = pv (ParamID::gateClose);
    s.gateReduction  = pv (ParamID::gateRed);
    s.gateOpenMs     = pv (ParamID::gateOpen);
    s.gateMinLenMs   = pv (ParamID::gateMinLen);
    s.breathOn       = pv (ParamID::breathOn) > 0.5f;
    s.breathReduction= pv (ParamID::breathRed);
    s.breathAuto     = pv (ParamID::breathMode) < 0.5f;
    s.breathThreshDb = pv (ParamID::breathThresh);
    s.breathTransMs  = pv (ParamID::breathTrans);
    s.sibOn          = pv (ParamID::sibOn) > 0.5f;
    s.sibAuto        = pv (ParamID::sibMode) < 0.5f;
    s.sibThreshDb    = pv (ParamID::sibThresh);
    s.sibTransMs     = pv (ParamID::sibTrans);
    s.sibSpectral    = pv (ParamID::sibSpectral) * 0.01f;
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
                                                        const VXAudioModification& mod,
                                                        const vx::Analysis& analysis)
{
    auto c = std::make_shared<vx::GainCurve> (base);
    const auto& edits = mod.editsDb;
    const bool hasNodes = ! mod.nodes.empty();
    const double frameSec = (double) c->hopSamples / c->sampleRate;
    bool any = false;
    for (size_t i = 0; i < c->gainDb.size(); ++i)
    {
        float e = i < edits.size() ? edits[i] : 0.0f;
        if (hasNodes) e += mod.nodeEnvAt (((double) i + 0.5) * frameSec);   // pincel + nos
        if (std::abs (e) > 1.0e-6f) any = true;
        const float g = jlimit (vx::kInfDb, 24.0f, c->autoDb[i] + e);
        c->gainDb[i]  = g;
        c->gainLin[i] = g <= vx::kInfDb + 0.01f ? 0.0f : vx::dbToLin (g);
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

        // copia de seguranca: usada so se o arquivo ARA do projeto nao trouxe nada para este clipe
        {
            const ScopedLock sl (backupLock);
            auto pb = pendingBackup.find (String (mod->getPersistentID()));
            if (pb != pendingBackup.end())
            {
                if (! mod->restoredFromArchive && ! mod->hasEdits() && mod->disabledRegions.empty())
                {
                    pb->second.applyTo (*mod);
                    mod->notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), false);
                }
                pendingBackup.erase (pb);
            }
        }

        auto analysis = src->getAnalysis();
        auto& e = cache[mod];

        if (analysis == nullptr)
        {
            if (e.full != nullptr) changed = true;
            e = CacheEntry();
            continue;
        }

        mod->ensureSize (analysis->numFrames());

        if (e.source != src || e.base == nullptr || e.analysisVersion != src->getAnalysisVersion()
            || e.settings != settings || e.regionsVersion != mod->regionsVersion)
        {
            e.source = src;
            e.base = vx::computeGainCurve (*analysis, settings, nullptr, &mod->disabledRegions);
            e.analysisVersion = src->getAnalysisVersion();
            e.regionsVersion = mod->regionsVersion;
            e.settings = settings;
            e.editsVersion = -1;
        }

        if (e.full == nullptr || e.editsVersion != mod->editsVersion)
        {
            e.full = applyEdits (*e.base, *mod, *analysis);
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
        updateBackup();
    }
}

std::shared_ptr<const vx::GainCurve> VXProcessor::getCurveFor (const ARAAudioModification* mod) const
{
    auto it = cache.find (mod);
    return it != cache.end() ? it->second.full : nullptr;
}

//==============================================================================
static const char* kBackupTag = "VXEditsBackup";

void VXProcessor::markDirty()
{
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void VXProcessor::updateBackup()
{
    auto* renderer = getRenderer();
    if (renderer == nullptr) return;

    bool changed = false;
    std::vector<VXAudioModification*> mods;
    for (auto* region : renderer->getPlaybackRegions())
        if (auto* m = dynamic_cast<VXAudioModification*> (region->getAudioModification()))
            if (std::find (mods.begin(), mods.end(), m) == mods.end())
            {
                mods.push_back (m);
                const auto v = std::make_pair (m->editsVersion, m->regionsVersion);
                auto it = backupVersions.find (m);
                if (it == backupVersions.end() || it->second != v) { backupVersions[m] = v; changed = true; }
            }

    if (! changed && backupXml != nullptr) return;

    auto xml = std::make_unique<XmlElement> (kBackupTag);
    for (auto* m : mods)
    {
        const auto st = VXModState::from (*m);
        if (st.isEmpty()) continue;
        MemoryOutputStream mo;
        st.write (mo);
        auto* clip = xml->createNewChildElement ("Clip");
        clip->setAttribute ("id", String (m->getPersistentID()));
        clip->setAttribute ("data", mo.getMemoryBlock().toBase64Encoding());
    }
    const ScopedLock sl (backupLock);
    backupXml = std::move (xml);
}

void VXProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->deleteAllChildElementsWithTagName (kBackupTag);
        {
            const ScopedLock sl (backupLock);
            if (backupXml != nullptr && backupXml->getNumChildElements() > 0)
                xml->addChildElement (new XmlElement (*backupXml));
        }
        copyXmlToBinary (*xml, dest);
    }
}

void VXProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        if (auto* bk = xml->getChildByName (kBackupTag))
        {
            const ScopedLock sl (backupLock);
            for (auto* clip : bk->getChildWithTagNameIterator ("Clip"))
            {
                MemoryBlock mb;
                if (! mb.fromBase64Encoding (clip->getStringAttribute ("data"))) continue;
                MemoryInputStream mi (mb, false);
                VXModState st;
                if (st.read (mi))
                    pendingBackup[clip->getStringAttribute ("id")] = std::move (st);
            }
            backupXml = std::make_unique<XmlElement> (*bk);
        }
        xml->deleteAllChildElementsWithTagName (kBackupTag);

        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
    }
}

AudioProcessorEditor* VXProcessor::createEditor()
{
    return new VXEditor (*this);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VXProcessor();
}
