#include "VXDocumentController.h"

using namespace juce;

//==============================================================================
//  Thread de analise
//==============================================================================
class VXAudioSource::AnalysisThread : public Thread
{
public:
    explicit AnalysisThread (VXAudioSource& o) : Thread ("VX_Leveler analysis"), owner (o) {}

    void run() override
    {
        auto* r = owner.reader.get();
        if (r == nullptr) { owner.state = failed; return; }

        const int64 total = owner.getSampleCount();
        const int   nch   = jlimit (1, 2, (int) owner.getChannelCount());
        const int   chunk = 1 << 15;

        vx::Analyzer analyzer (owner.getSampleRate(), nch, total);
        AudioBuffer<float> buffer (nch, chunk);

        for (int64 pos = 0; pos < total; pos += chunk)
        {
            if (threadShouldExit()) { owner.state = needsAnalysis; return; }

            const int n = (int) jmin<int64> (chunk, total - pos);
            buffer.clear();

            bool ok = r->read (&buffer, 0, n, pos, true, nch > 1);
            if (! ok)
            {
                // o host pode estar ocupado: tenta de novo algumas vezes
                for (int attempt = 0; attempt < 20 && ! ok && ! threadShouldExit(); ++attempt)
                {
                    wait (50);
                    ok = r->read (&buffer, 0, n, pos, true, nch > 1);
                }
                if (! ok) { owner.state = failed; return; }
            }

            analyzer.process (buffer.getArrayOfReadPointers(), nch, n);
            owner.progress = (float) ((double) (pos + n) / (double) jmax<int64> (1, total));
        }

        std::shared_ptr<const vx::Analysis> result = analyzer.finish();
        std::atomic_store (&owner.analysis, result);
        owner.analysisVersion.fetch_add (1);
        owner.progress = 1.0f;
        owner.state = ready;
    }

private:
    VXAudioSource& owner;
};

//==============================================================================
VXAudioSource::VXAudioSource (ARA::PlugIn::Document* doc, ARA::ARAAudioSourceHostRef hostRef)
    : ARAAudioSource (doc, hostRef)
{
}

VXAudioSource::~VXAudioSource()
{
    stopAnalysis();
}

String VXAudioSource::getDisplayName() const
{
    return convertOptionalARAString (getName(), "Audio");
}

void VXAudioSource::stopAnalysis()
{
    if (thread != nullptr)
    {
        thread->signalThreadShouldExit();
        thread->stopThread (8000);
        thread.reset();
    }
    reader.reset();
}

void VXAudioSource::startAnalysisIfNeeded()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // thread terminou: libera leitor/thread na thread de mensagens
    if (thread != nullptr && ! thread->isThreadRunning())
    {
        thread.reset();
        reader.reset();
    }

    const auto s = getState();
    if (s == analysing || s == ready)
        return;

    if (s == failed)
    {
        if (retries >= 3) return;
        ++retries;
    }

    if (! isSampleAccessEnabled() || getSampleCount() <= 0 || getSampleRate() <= 0.0)
        return;

    stopAnalysis();
    reader = std::make_unique<ARAAudioSourceReader> (this);
    if (! reader->isValid())
    {
        reader.reset();
        return;
    }

    progress = 0.0f;
    state = analysing;
    thread = std::make_unique<AnalysisThread> (*this);
    thread->startThread (Thread::Priority::low);
}

void VXAudioSource::invalidateAnalysis()
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopAnalysis();
    std::atomic_store (&analysis, std::shared_ptr<const vx::Analysis>());
    analysisVersion.fetch_add (1);
    retries = 0;
    state = needsAnalysis;
}

void VXAudioSource::setRestoredAnalysis (std::shared_ptr<const vx::Analysis> a)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (a == nullptr) return;
    stopAnalysis();
    std::atomic_store (&analysis, a);
    analysisVersion.fetch_add (1);
    retries = 0;
    progress = 1.0f;
    state = ready;
}

//==============================================================================
VXModState VXModState::from (const VXAudioModification& m)
{
    VXModState st;
    st.edits = m.editsDb;
    st.disabled = m.disabledRegions;
    st.nodes = m.nodes;
    return st;
}

void VXModState::applyTo (VXAudioModification& m) const
{
    m.editsDb = edits;
    m.disabledRegions = disabled;
    m.nodes = nodes;
    ++m.editsVersion;
    ++m.regionsVersion;
}

bool VXModState::isEmpty() const
{
    for (auto v : edits) if (std::abs (v) > 1.0e-6f) return false;
    return disabled.empty() && nodes.empty();
}

void VXModState::write (OutputStream& out) const
{
    // edicoes do pincel em trechos nao nulos: [inicio, tamanho, valores...]
    std::vector<std::pair<int, int>> runs;
    for (int i = 0; i < (int) edits.size();)
    {
        if (std::abs (edits[(size_t) i]) <= 1.0e-6f) { ++i; continue; }
        int j = i;
        while (j < (int) edits.size() && std::abs (edits[(size_t) j]) > 1.0e-6f) ++j;
        runs.push_back ({ i, j - i });
        i = j;
    }
    out.writeInt ((int) edits.size());
    out.writeInt ((int) runs.size());
    for (auto& r : runs)
    {
        out.writeInt (r.first);
        out.writeInt (r.second);
        out.write (edits.data() + r.first, (size_t) r.second * sizeof (float));
    }
    out.writeInt ((int) disabled.size());
    for (auto& k : disabled) { out.writeInt (k.type); out.writeInt (k.startFrame); }
    out.writeInt ((int) nodes.size());
    for (auto& n : nodes) { out.writeDouble (n.t); out.writeFloat (n.db); }
}

bool VXModState::read (InputStream& in)
{
    const int total = in.readInt();
    if (total < 0 || total > 100000000) return false;
    edits.assign ((size_t) total, 0.0f);
    const int nRuns = in.readInt();
    for (int r = 0; r < nRuns && ! in.isExhausted(); ++r)
    {
        const int start = in.readInt(), len = in.readInt();
        if (start < 0 || len < 0 || start + len > total) return false;
        if (in.read (edits.data() + start, (int) ((size_t) len * sizeof (float))) != (int) ((size_t) len * sizeof (float)))
            return false;
    }
    const int nd = in.readInt();
    if (nd < 0 || nd > 1000000) return false;
    disabled.clear();
    for (int k = 0; k < nd; ++k) { vx::RegionKey key; key.type = in.readInt(); key.startFrame = in.readInt(); disabled.push_back (key); }
    const int nn = in.readInt();
    if (nn < 0 || nn > 1000000) return false;
    nodes.clear();
    for (int k = 0; k < nn; ++k) { VXAudioModification::Node n; n.t = in.readDouble(); n.db = in.readFloat(); nodes.push_back (n); }
    return true;
}

//==============================================================================
static constexpr int kAnalysisMagic = 0x56584132;   // "VXA2" (valores quantizados + gzip)

namespace
{
    // energia/pico em dB com passo de 0,01 dB em 16 bits
    inline int16 encDb (float lin, bool power) noexcept
    {
        const float db = power ? 10.0f * std::log10 (lin + 1.0e-14f) : 20.0f * std::log10 (lin + 1.0e-9f);
        return (int16) jlimit (-32000, 32000, roundToInt (db * 100.0f));
    }
    inline float decDb (int16 v, bool power) noexcept
    {
        const float db = (float) v * 0.01f;
        return power ? std::pow (10.0f, db * 0.1f) : std::pow (10.0f, db * 0.05f);
    }
}

MemoryBlock vxSerialiseAnalysis (const vx::Analysis& a)
{
    MemoryBlock block;
    {
        MemoryOutputStream mo (block, false);
        GZIPCompressorOutputStream gz (mo, 9);
        gz.writeInt (kAnalysisMagic);
        gz.writeDouble (a.sampleRate);
        gz.writeInt (a.hopSamples);
        gz.writeInt64 (a.numSamples);
        gz.writeInt (a.numChannels);
        const int n = a.numFrames();
        gz.writeInt (n);
        const bool feat = (int) a.voicing.size() == n && (int) a.zcr.size() == n && (int) a.totMs.size() == n
                       && (int) a.hfMs.size() == n && (int) a.sibFreq.size() == n;
        gz.writeBool (feat);
        // cada serie gravada inteira (comprime melhor do que intercalada)
        for (int i = 0; i < n; ++i) gz.writeShort (encDb (a.ms[(size_t) i], true));
        for (int i = 0; i < n; ++i) gz.writeShort (encDb (a.peak[(size_t) i], false));
        if (feat)
        {
            for (int i = 0; i < n; ++i) gz.writeByte ((char) (uint8) jlimit (0, 255, roundToInt (a.voicing[(size_t) i] * 250.0f)));
            for (int i = 0; i < n; ++i) gz.writeShort ((short) (uint16) jlimit (0, 65535, roundToInt (a.zcr[(size_t) i] * 0.25f)));
            for (int i = 0; i < n; ++i) gz.writeShort (encDb (a.totMs[(size_t) i], true));
            for (int i = 0; i < n; ++i) gz.writeShort (encDb (a.hfMs[(size_t) i], true));
            for (int i = 0; i < n; ++i) gz.writeShort ((short) (uint16) jlimit (0, 65535, roundToInt (a.sibFreq[(size_t) i] * 0.02f)));
        }
        gz.flush();
    }
    return block;
}

std::shared_ptr<vx::Analysis> vxDeserialiseAnalysis (const void* data, size_t size)
{
    MemoryInputStream mi (data, size, false);
    GZIPDecompressorInputStream gz (mi);
    if (gz.readInt() != kAnalysisMagic) return nullptr;
    auto a = std::make_shared<vx::Analysis>();
    a->sampleRate  = gz.readDouble();
    a->hopSamples  = gz.readInt();
    a->numSamples  = gz.readInt64();
    a->numChannels = gz.readInt();
    const int n = gz.readInt();
    if (n < 0 || n > 200000000 || a->hopSamples <= 0) return nullptr;
    const bool feat = gz.readBool();

    a->ms.resize ((size_t) n);   a->peak.resize ((size_t) n);
    for (int i = 0; i < n; ++i) a->ms[(size_t) i]   = decDb ((int16) gz.readShort(), true);
    for (int i = 0; i < n; ++i) a->peak[(size_t) i] = decDb ((int16) gz.readShort(), false);
    if (feat)
    {
        for (auto* v : { &a->voicing, &a->zcr, &a->totMs, &a->hfMs, &a->sibFreq }) v->resize ((size_t) n);
        for (int i = 0; i < n; ++i) a->voicing[(size_t) i] = (float) (uint8) gz.readByte() / 250.0f;
        for (int i = 0; i < n; ++i) a->zcr[(size_t) i]     = (float) (uint16) gz.readShort() * 4.0f;
        for (int i = 0; i < n; ++i) a->totMs[(size_t) i]   = decDb ((int16) gz.readShort(), true);
        for (int i = 0; i < n; ++i) a->hfMs[(size_t) i]    = decDb ((int16) gz.readShort(), true);
        for (int i = 0; i < n; ++i) a->sibFreq[(size_t) i] = (float) (uint16) gz.readShort() * 50.0f;
    }
    if (gz.isExhausted() && n > 0 && a->ms.empty()) return nullptr;
    return a;
}

//==============================================================================
VXAudioModification::VXAudioModification (ARAAudioSource* audioSource,
                                          ARA::ARAAudioModificationHostRef hostRef,
                                          const ARAAudioModification* clone)
    : ARAAudioModification (audioSource, hostRef, clone)
{
    if (auto* c = dynamic_cast<const VXAudioModification*> (clone))
    {
        editsDb = c->editsDb;
        disabledRegions = c->disabledRegions;
        nodes = c->nodes;
    }
}

void VXAudioModification::notifyEdited()
{
    ++editsVersion;
    notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), true);
    for (auto* region : getPlaybackRegions())
        region->notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), true);
}

String VXAudioModification::getDisplayName() const
{
    auto name = convertOptionalARAString (getEffectiveName());
    if (name.isEmpty())
        if (auto* src = getAudioSource<VXAudioSource>())
            name = src->getDisplayName();
    return name;
}

//==============================================================================
//  Renderer
//==============================================================================
VXPlaybackRenderer::VXPlaybackRenderer (ARA::PlugIn::DocumentController* dc, VXDocumentController& o)
    : ARAPlaybackRenderer (dc), owner (o)
{
}

void VXPlaybackRenderer::prepareToPlay (double sr, int maxBlockIn, int numChannelsIn,
                                        AudioProcessor::ProcessingPrecision precision,
                                        AlwaysNonRealtime alwaysNonRealtime)
{
    ARAPlaybackRenderer::prepareToPlay (sr, maxBlockIn, numChannelsIn, precision, alwaysNonRealtime);

    sampleRate  = sr;
    maxBlock    = maxBlockIn;
    numChannels = numChannelsIn;
    tempBuffer  = std::make_unique<AudioBuffer<float>> (numChannels, maxBlock);

    const bool buffered = alwaysNonRealtime == AlwaysNonRealtime::no;
    bool mismatch = false;

    deEssers.clear();
    for (auto* region : getPlaybackRegions())
    {
        auto& d = deEssers[region];
        d.filter.prepare (sampleRate, numChannels);
        d.expectedSource = -1;

        auto* source = region->getAudioModification()->getAudioSource();
        if (! approximatelyEqual (source->getSampleRate(), sampleRate)) mismatch = true;

        if (readers.find (source) != readers.end())
            continue;

        auto reader = std::make_unique<ARAAudioSourceReader> (source);
        Reader r;
        if (buffered)
        {
            const int readAhead = jmax (4 * maxBlock, roundToInt (2.0 * sampleRate));
            auto* b = new BufferingAudioReader (reader.release(), *readThread, readAhead);
            r.buffering = b;
            r.reader.reset (b);
        }
        else
        {
            r.reader = std::move (reader);
        }
        readers.emplace (source, std::move (r));
    }
    sampleRateMismatch = mismatch;
}

void VXPlaybackRenderer::releaseResources()
{
    readers.clear();
    deEssers.clear();
    tempBuffer.reset();
    ARAPlaybackRenderer::releaseResources();
}

void VXPlaybackRenderer::publishCurves (std::shared_ptr<const VXCurveMap> map)
{
    JUCE_ASSERT_MESSAGE_THREAD
    keepAlive.push_back (map);
    while (keepAlive.size() > 8) keepAlive.pop_front();
    std::atomic_store (&curves, map);
}

bool VXPlaybackRenderer::processBlock (AudioBuffer<float>& buffer,
                                       AudioProcessor::Realtime realtime,
                                       const AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const auto lock = owner.getProcessingLock();
    if (! lock.isLocked() || tempBuffer == nullptr)
    {
        buffer.clear();
        return true;
    }

    const int numSamples = buffer.getNumSamples();
    const int nch = jmin (buffer.getNumChannels(), tempBuffer->getNumChannels());
    if (numSamples > tempBuffer->getNumSamples())
    {
        buffer.clear();
        return false;
    }

    buffer.clear();
    if (! positionInfo.getIsPlaying())
        return true;

    const auto curveMap = std::atomic_load (&curves);
    const bool bypass = bypassed.load();
    const float outGain = outputGain.load();
    const int64 timeInSamples = positionInfo.getTimeInSamples().orFallback (0);
    const auto blockRange = Range<int64>::withStartAndLength (timeInSamples, numSamples);

    bool success = true;
    float lastGain = 0.0f;
    bool anyGain = false;

    for (auto* region : getPlaybackRegions())
    {
        const auto playbackRange = region->getSampleRange (sampleRate, ARAPlaybackRegion::IncludeHeadAndTail::no);
        auto renderRange = blockRange.getIntersectionWith (playbackRange);
        if (renderRange.isEmpty()) continue;

        const Range<int64> modRange { region->getStartInAudioModificationSamples(),
                                      region->getEndInAudioModificationSamples() };
        const int64 offset = modRange.getStart() - playbackRange.getStart();
        renderRange = renderRange.getIntersectionWith (modRange.movedToStartAt (playbackRange.getStart()));
        if (renderRange.isEmpty()) continue;

        auto* mod = region->getAudioModification();
        auto* source = mod->getAudioSource();
        auto it = readers.find (source);
        if (it == readers.end()) { success = false; continue; }

        auto& rd = it->second;
        if (rd.buffering != nullptr)
            rd.buffering->setReadTimeout (realtime == AudioProcessor::Realtime::no ? 200 : 0);

        const int n = (int) renderRange.getLength();
        const int startInBuffer = (int) (renderRange.getStart() - blockRange.getStart());
        const int64 startInSource = renderRange.getStart() + offset;

        if (! rd.reader->read (tempBuffer.get(), startInBuffer, n, startInSource, true, true))
        {
            success = false;
            continue;
        }

        // curva de ganho do clipe
        const vx::GainCurve* curve = nullptr;
        if (curveMap != nullptr && ! bypass)
        {
            auto ci = curveMap->find (mod);
            if (ci != curveMap->end() && ci->second != nullptr) curve = ci->second.get();
        }

        // posicao na fonte -> amostras da curva (mesma taxa da fonte)
        const double srcScale = curve != nullptr ? curve->sampleRate / sampleRate : 1.0;

        // parte espectral do de-esser
        if (curve != nullptr && curve->hasSpectral)
        {
            auto di = deEssers.find (region);
            if (di != deEssers.end())
            {
                auto& d = di->second;
                if (d.expectedSource != startInSource) d.filter.reset();
                d.filter.process (tempBuffer->getArrayOfWritePointers(), nch, startInBuffer, n,
                                  *curve, (double) startInSource * srcScale, srcScale);
                d.expectedSource = startInSource + n;
            }
        }

        for (int c = 0; c < nch; ++c)
        {
            auto* src = tempBuffer->getReadPointer (c, startInBuffer);
            auto* dst = buffer.getWritePointer (c, startInBuffer);

            if (curve != nullptr)
            {
                for (int i = 0; i < n; ++i)
                    dst[i] += src[i] * curve->gainAtSample ((double) (startInSource + i) * srcScale) * outGain;
            }
            else
            {
                for (int i = 0; i < n; ++i)
                    dst[i] += src[i] * (bypass ? 1.0f : outGain);
            }
        }

        if (curve != nullptr)
        {
            lastGain = 20.0f * std::log10 (jmax (1.0e-6f, curve->gainAtSample ((double) startInSource * srcScale)));
            anyGain = true;
        }
    }

    currentGainDb = anyGain ? lastGain : 0.0f;
    return success;
}

//==============================================================================
//  Document controller
//==============================================================================
class VXAnalysisScheduler : public Timer
{
public:
    explicit VXAnalysisScheduler (VXDocumentController& o) : owner (o) {}
    ~VXAnalysisScheduler() override { stopTimer(); }
    void timerCallback() override { owner.scheduleAnalyses(); }
private:
    VXDocumentController& owner;
};

VXDocumentController::VXDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                                            const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation (entry, instance),
      scheduler (std::make_unique<VXAnalysisScheduler> (*this))
{
}

VXDocumentController::~VXDocumentController()
{
    scheduler.reset();
}

void VXDocumentController::willBeginEditing (ARADocument* doc)
{
    processLock.enterWrite();
    document = doc;
    isEditing = true;
}

void VXDocumentController::didEndEditing (ARADocument* doc)
{
    document = doc;
    isEditing = false;
    processLock.exitWrite();
    ++modelVersion;
    scheduleAnalyses();
    if (scheduler != nullptr && ! scheduler->isTimerRunning()) scheduler->startTimer (300);
}

ARAAudioSource* VXDocumentController::doCreateAudioSource (ARADocument* doc, ARA::ARAAudioSourceHostRef hostRef) noexcept
{
    return new VXAudioSource (doc, hostRef);
}

ARAAudioModification* VXDocumentController::doCreateAudioModification (ARAAudioSource* audioSource,
                                                                       ARA::ARAAudioModificationHostRef hostRef,
                                                                       const ARAAudioModification* clone) noexcept
{
    return new VXAudioModification (audioSource, hostRef, clone);
}

ARAPlaybackRenderer* VXDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new VXPlaybackRenderer (getDocumentController(), *this);
}

void VXDocumentController::doUpdateAudioSourceContent (ARAAudioSource* audioSource, ARAContentUpdateScopes scopeFlags)
{
    if (scopeFlags.affectSamples())
        if (auto* s = dynamic_cast<VXAudioSource*> (audioSource))
            s->invalidateAnalysis();
    ++modelVersion;
}

void VXDocumentController::didEnableAudioSourceSamplesAccess (ARAAudioSource*, bool enable)
{
    if (enable && ! isEditing)
        scheduleAnalyses();
}

void VXDocumentController::scheduleAnalyses()
{
    if (document == nullptr || isEditing) return;
    for (auto* s : document->getAudioSources())
        if (auto* vs = dynamic_cast<VXAudioSource*> (s))
            vs->startAnalysisIfNeeded();
}


void VXDocumentController::reanalyseAll()
{
    if (document == nullptr || isEditing) return;
    for (auto* s : document->getAudioSources())
        if (auto* vs = dynamic_cast<VXAudioSource*> (s))
            vs->invalidateAnalysis();
    scheduleAnalyses();
}

String VXDocumentController::getDiagnostics() const
{
    if (document == nullptr)
        return "documento ARA ainda nao recebido do host";

    int total = 0, access = 0, ready = 0, running = 0, failedCount = 0;
    for (auto* s : document->getAudioSources())
    {
        ++total;
        if (s->isSampleAccessEnabled()) ++access;
        if (auto* vs = dynamic_cast<VXAudioSource*> (s))
        {
            const auto st = vs->getState();
            if (st == VXAudioSource::ready) ++ready;
            else if (st == VXAudioSource::analysing) ++running;
            else if (st == VXAudioSource::failed) ++failedCount;
        }
    }
    return "fontes: " + String (total) + " | acesso: " + String (access)
         + " | analisadas: " + String (ready) + " | analisando: " + String (running)
         + " | falhas: " + String (failedCount);
}

//------------------------------------------------------------------------------
static constexpr int kArchiveMagic = 0x56584c31;   // "VXL1"

bool VXDocumentController::doStoreObjectsToStream (ARAOutputStream& output, const ARAStoreObjectsFilter* filter) noexcept
{
    // versao 4: [clipes: estado manual] + [fontes: analise compactada]
    const auto& mods = filter->getAudioModificationsToStore<VXAudioModification>();
    const auto& sources = filter->getAudioSourcesToStore<VXAudioSource>();
    auto* archiving = getDocumentController()->getHostArchivingController();
    const float total = (float) jmax<size_t> (1, mods.size() + sources.size());
    float done = 0.0f;

    if (! output.writeInt (kArchiveMagic)) return false;
    if (! output.writeInt (4)) return false;

    if (! output.writeInt64 ((int64) mods.size())) return false;
    for (auto* m : mods)
    {
        MemoryOutputStream state;
        VXModState::from (*m).write (state);
        if (! output.writeString (String (m->getPersistentID()))) return false;
        if (! output.writeInt ((int) state.getDataSize())) return false;
        if (state.getDataSize() > 0 && ! output.write (state.getData(), state.getDataSize())) return false;
        archiving->notifyDocumentArchivingProgress (++done / total);
    }

    if (! output.writeInt64 ((int64) sources.size())) return false;
    for (auto* src : sources)
    {
        if (! output.writeString (String (src->getPersistentID()))) return false;
        auto analysis = src->getAnalysis();
        if (analysis == nullptr || src->getState() != VXAudioSource::ready)
        {
            if (! output.writeInt (0)) return false;
        }
        else
        {
            const auto block = vxSerialiseAnalysis (*analysis);
            if (! output.writeInt ((int) block.getSize())) return false;
            if (! output.write (block.getData(), block.getSize())) return false;
        }
        archiving->notifyDocumentArchivingProgress (++done / total);
    }

    archiving->notifyDocumentArchivingProgress (1.0f);
    return true;
}

bool VXDocumentController::doRestoreObjectsFromStream (ARAInputStream& input, const ARARestoreObjectsFilter* filter) noexcept
{
    if (input.readInt() != kArchiveMagic) return ! input.failed();
    const int version = input.readInt();
    const auto count = input.readInt64();
    auto* archiving = getDocumentController()->getHostArchivingController();

    for (int64 i = 0; i < count && ! input.failed(); ++i)
    {
        const String persistentID = input.readString();
        VXModState st;

        if (version >= 4)
        {
            const int size = input.readInt();
            if (size < 0 || size > 400000000) return false;
            MemoryBlock block ((size_t) size);
            if (size > 0 && input.read (block.getData(), size) != size) return false;
            MemoryInputStream mi (block, false);
            if (size > 0 && ! st.read (mi)) continue;
        }
        else
        {
            // formatos antigos (1 a 3)
            const int n = input.readInt();
            if (n > 0 && n < 100000000)
            {
                st.edits.resize ((size_t) n);
                if (input.read (st.edits.data(), (int) ((size_t) n * sizeof (float))) != (int) ((size_t) n * sizeof (float)))
                    return false;
            }
            if (version >= 2)
            {
                const int nk = input.readInt();
                for (int k = 0; k < nk && k < 1000000 && ! input.failed(); ++k)
                {
                    vx::RegionKey key;
                    key.type = input.readInt();
                    key.startFrame = input.readInt();
                    st.disabled.push_back (key);
                }
            }
            if (version >= 3)
            {
                const int nn = input.readInt();
                for (int k = 0; k < nn && k < 1000000 && ! input.failed(); ++k)
                {
                    VXAudioModification::Node nd;
                    nd.t  = input.readDouble();
                    nd.db = input.readFloat();
                    st.nodes.push_back (nd);
                }
            }
        }

        if (auto* m = filter->getAudioModificationToRestoreStateWithID<VXAudioModification> (persistentID.getCharPointer()))
        {
            st.applyTo (*m);
            m->restoredFromArchive = true;
            m->notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), false);
        }
        archiving->notifyDocumentUnarchivingProgress (0.5f * (float) (i + 1) / (float) jmax<int64> (1, count));
    }

    if (version >= 4 && ! input.failed())
    {
        const auto nSources = input.readInt64();
        for (int64 i = 0; i < nSources && ! input.failed(); ++i)
        {
            const String persistentID = input.readString();
            const int size = input.readInt();
            if (size < 0 || size > 400000000) return false;
            if (size == 0) continue;
            MemoryBlock block ((size_t) size);
            if (input.read (block.getData(), size) != size) return false;

            if (auto* src = filter->getAudioSourceToRestoreStateWithID<VXAudioSource> (persistentID.getCharPointer()))
            {
                auto a = vxDeserialiseAnalysis (block.getData(), block.getSize());
                // so usa se o audio for o mesmo (tamanho, taxa e canais)
                if (a != nullptr && a->numSamples == src->getSampleCount()
                    && std::abs (a->sampleRate - src->getSampleRate()) < 0.5
                    && a->numChannels == jlimit (1, 2, (int) src->getChannelCount()))
                    src->setRestoredAnalysis (a);
            }
            archiving->notifyDocumentUnarchivingProgress (0.5f + 0.5f * (float) (i + 1) / (float) jmax<int64> (1, nSources));
        }
    }

    archiving->notifyDocumentUnarchivingProgress (1.0f);
    return ! input.failed();
}

//==============================================================================
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<VXDocumentController>();
}
